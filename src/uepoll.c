/*
**  LICENSE: MIT
**  Author: CandyMi[https://github.com/candymi]
**
**  uepoll.c — select(2)-based epoll emulation for generic POSIX systems.
**
**  Falls back to select(2) when neither kernel epoll nor kqueue is
**  available.  Limited to FD_SETSIZE (default 1024) fds.
**
**  Key design:
**    • 3 × fd_set (rset / wset / eset) + udata[FD_SETSIZE] array
**    • Self-waking pipe for cross-thread epoll_ctl → epoll_wait wake
**    • Spinlock (3-level degrading) for thread safety
**    • ONESHOT support via fd_set removal after first event
**
**  Limitations:
**    • EPOLLET / EPOLLHUP unsupported (select cannot detect)
**    • EPOLLPRI mapped through the error fd_set (select(2) cannot
**      distinguish OOB data from error conditions — both report via
**      EPOLLPRI if registered, EPOLLERR otherwise)
**    • fd capped at FD_SETSIZE
**    • Timeout converted from ms → timeval each call
**
**  This is the original universal fallback; ppoll.c is the recommended
**  replacement on any system with poll(2).
*/

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif

#include "uepoll.h"
#include "epoll_internal.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>

#ifndef FD_COPY
  #define FD_COPY(f, t)  memcpy(t, f, sizeof(*f));
#endif

#define EPOLL_MAX_EVENTS FD_SETSIZE

#define EPOLLEMPTY  (0)
#define EPOLL_SUCCESS  (0)

#define uepoll_has_events(e)    (e & (EPOLLIN | EPOLLOUT | EPOLLERR))
#define uepoll_has_oneshot(e)   (e & EPOLLONESHOT)

#define epoll_notice(ep)  { ep->edited = true; write((ep)->pipes[1], "x", 1); }

struct epoll_t
{
  bool edited;
  int nfds;
  int pipes[2];
  epoll_lock_t lock;
  volatile int closing;
  fd_set rset;
  fd_set wset;
  fd_set eset;
  epoll_event udata[EPOLL_MAX_EVENTS];
};

static inline void epoll_receive(struct epoll_t *ep) {
    if (ep->edited) {
        ep->edited = false;
        char buffer[1024];
        while (read(ep->pipes[0], buffer, sizeof(buffer)) > 0) {}
    }
}

static epoll_realloc_t epoll_realloc = realloc;
void epoll_allocator(epoll_realloc_t realloc_func)
{
  if (!realloc_func) return;
  epoll_realloc = realloc_func;
}

static inline
struct timeval* uepoll_timeout2timeval(int timeout, struct timeval* tv, int64_t *now)
{
  if (timeout >= 0) {
    tv->tv_sec = timeout / 1000;
    tv->tv_usec = (timeout % 1000) * 1000;
    if (now) *now = epoll_now_ms();
    return tv;
  }
  return NULL;
}

static inline
bool uepoll_can_poll(SOCKET fd) {
  struct stat st;
  int r = fstat(fd, &st); // printf("r = %d, mode = %d\n", r, st.st_mode);
  if (r)
    return false;
  return S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode) || S_ISREG(st.st_mode);
}

static inline
bool uepoll_has_fd(struct epoll_t* ep, SOCKET fd)
{
  return ep->udata[fd]._nouse == 0;
}

static inline
void uepoll_append_nfds(struct epoll_t* ep, SOCKET fd)
{
  // printf("ep->nfds: %d, fd = %d", ep->nfds, fd);
  if (ep->nfds <= fd)
    ep->nfds = fd + 1;
}

static inline
void uepoll_update_nfds(struct epoll_t* ep, SOCKET fd)
{
  /* find next max-fd */
  if (fd + 1 != ep->nfds)
    return;
  fd = ep->nfds - 1;
  for (; fd >= 0; fd--)
  {
    if (uepoll_has_fd(ep, fd))
    {
      if (FD_ISSET(fd, &ep->rset) || FD_ISSET(fd, &ep->wset) || FD_ISSET(fd, &ep->eset))
      {
        ep->nfds = fd + 1;
        return;
      }
    }
  }
  ep->nfds = 0;
  return;
}

/* Lock-held variants — assume ep->lock is held by epoll_ctl.
 * EPOLLERR is always enabled (Linux epoll guarantees this). */
static inline
int uepoll_add_locked(struct epoll_t* ep, SOCKET fd, struct epoll_event* ev)
{
  if (ep->closing) {
    errno = EBADF;
    return EPOLL_INVALID;
  }
  if (uepoll_has_fd(ep, fd)) {
    errno = EEXIST;
    return EPOLL_INVALID;
  }

  ep->udata[fd]._nouse = 0;
  ep->udata[fd].events = EPOLLEMPTY;
  ep->udata[fd].data.ptr = NULL;

  if (ev->events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND)) {
    FD_SET(fd, &ep->rset);
    ep->udata[fd].events |= EPOLLIN;
  }

  if (ev->events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) {
    FD_SET(fd, &ep->wset);
    ep->udata[fd].events |= EPOLLOUT;
  }

  /* Always monitor EPOLLERR — matches Linux epoll semantics. */
  FD_SET(fd, &ep->eset);
  ep->udata[fd].events |= EPOLLERR;

  /* EPOLLPRI also maps to eset (select(2) reports OOB data as
   * exceptional condition).  Both use the same fd_set so select
   * cannot distinguish EPOLLPRI from EPOLLERR; uepoll_wait checks
   * the registered flags to report the right one. */
  if (ev->events & EPOLLPRI) {
    FD_SET(fd, &ep->eset);
    ep->udata[fd].events |= EPOLLPRI;
  }

  if (uepoll_has_oneshot(ev->events)) {
    ep->udata[fd].events |= EPOLLONESHOT;
  }

  if (uepoll_has_events(ep->udata[fd].events)) {
    ep->udata[fd].data.ptr = ev->data.ptr;
    uepoll_append_nfds(ep, fd);
    epoll_notice(ep);
  }
  return EPOLL_SUCCESS;
}

static inline 
int uepoll_mod_locked(struct epoll_t* ep, int fd, struct epoll_event *ev)
{
  if (ep->closing) {
    errno = EBADF;
    return EPOLL_INVALID;
  }
  if (!uepoll_has_fd(ep, fd)) {
    errno = ENOENT;
    return EPOLL_INVALID;
  }

  ep->udata[fd].events = EPOLLEMPTY;
  ep->udata[fd].data.ptr = NULL;

  if (ev->events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND)) {
    FD_SET(fd, &ep->rset);
    ep->udata[fd].events |= EPOLLIN;
  } else {
    FD_CLR(fd, &ep->rset);
  }

  if (ev->events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) {
    FD_SET(fd, &ep->wset);
    ep->udata[fd].events |= EPOLLOUT;
  } else {
    FD_CLR(fd, &ep->wset);
  }

  /* Always monitor EPOLLERR — matches Linux epoll semantics. */
  FD_SET(fd, &ep->eset);
  ep->udata[fd].events |= EPOLLERR;

  if (ev->events & EPOLLPRI) {
    FD_SET(fd, &ep->eset);
    ep->udata[fd].events |= EPOLLPRI;
  }

  if (uepoll_has_oneshot(ev->events)) {
    ep->udata[fd].events |= EPOLLONESHOT;
  }

  ep->udata[fd].data.ptr = ev->data.ptr;
  epoll_notice(ep);
  uepoll_append_nfds(ep, fd);
  return EPOLL_SUCCESS;
}

static inline
int uepoll_del_locked(struct epoll_t* ep, SOCKET fd)
{
  if (ep->closing) {
    errno = EBADF;
    return EPOLL_INVALID;
  }
  if (!uepoll_has_fd(ep, fd)) {
    errno = ENOENT;
    return EPOLL_INVALID;
  }

  FD_CLR(fd, &ep->rset);
  FD_CLR(fd, &ep->wset);
  FD_CLR(fd, &ep->eset);

  ep->udata[fd]._nouse = 1;
  ep->udata[fd].events = EPOLLEMPTY;
  ep->udata[fd].data.ptr = NULL;

  epoll_notice(ep);
  uepoll_update_nfds(ep, fd);
  return 0;
}

/* Public wrappers — take lock, delegate, release */
static inline
int uepoll_add(struct epoll_t* ep, SOCKET fd, struct epoll_event* ev)
{
  epoll_spinlock_lock(&ep->lock);
  int r = uepoll_add_locked(ep, fd, ev);
  epoll_spinlock_unlock(&ep->lock);
  return r;
}

int epoll_ctl(HANDLE efd, int op, SOCKET fd, struct epoll_event *event)
{
  errno = 0;

  if (efd <= 0 || fd < 0) {
    errno = EBADF;
    return EPOLL_INVALID;
  }

  if (fd < 0 || fd >= EPOLL_MAX_EVENTS) {
    errno = ENOSPC;
    return EPOLL_INVALID;
  }

  if (op != EPOLL_CTL_DEL && !event) {
    errno = EFAULT;
    return EPOLL_INVALID;
  }

  if (!uepoll_can_poll(fd)) {
    errno = EPERM;
    return EPOLL_INVALID;
  }

  /* Take lock FIRST — prevents the TOCTOU race where epoll_close
   * could free ep between a pre-check and the internal lock acquisition
   * in the backend functions.  Locked variants assume lock held. */
  struct epoll_t* ep = (struct epoll_t*)efd;
  epoll_spinlock_lock(&ep->lock);

  int result;
  switch (op)
  {
    case EPOLL_CTL_ADD: result = uepoll_add_locked(ep, fd, event); break;
    case EPOLL_CTL_DEL: result = uepoll_del_locked(ep, fd); break;
    case EPOLL_CTL_MOD: result = uepoll_mod_locked(ep, fd, event); break;
    default: errno = EINVAL; result = EPOLL_INVALID; break;
  }

  epoll_spinlock_unlock(&ep->lock);
  return result;
}

int epoll_close(HANDLE efd)
{
  if (efd <= 0) {
    errno = EBADF; // 指针不会为负数.
    return EPOLL_INVALID;
  }

  struct epoll_t* ep = (struct epoll_t*)efd;

  epoll_spinlock_lock(&ep->lock);
  if (ep->closing) {
    epoll_spinlock_unlock(&ep->lock);
    errno = EBADF;
    return EPOLL_INVALID;
  }
  ep->closing = 1;
  /* Wake any select() blocked in epoll_wait */
  write(ep->pipes[1], "x", 1);
  epoll_spinlock_unlock(&ep->lock);

  close(ep->pipes[0]); ep->pipes[0] = -1;
  close(ep->pipes[1]); ep->pipes[1] = -1;
  epoll_free(ep);
  return EPOLL_SUCCESS;
}

HANDLE epoll_create(int size)
{
  if (size < 0) {
    errno = EINVAL;
    return -1;
  }
  return epoll_create1(0);
}

HANDLE epoll_create1(int flags)
{
  if (flags && flags != EPOLL_CLOEXEC) {
    errno = EINVAL;
    return -1;
  }
  errno = 0;
  struct epoll_t *ep = (struct epoll_t *)epoll_malloc(sizeof(struct epoll_t));
  if (!ep) {
    errno = ENOMEM;
    return -1;
  }
  memset(ep, 0, sizeof(struct epoll_t));
  ep->pipes[0] = ep->pipes[1] = -1;
  if (pipe(ep->pipes)) {
    epoll_close((HANDLE)ep);
    return -1;
  }
  if (flags & EPOLL_CLOEXEC) {
    epoll_nonexec(ep->pipes[0]);
    epoll_nonexec(ep->pipes[1]);
  }
  epoll_nonblock(ep->pipes[0]);
  epoll_nonblock(ep->pipes[1]);
  /* 初始化 */
  ep->nfds = 0;
  ep->edited = false;
  FD_ZERO(&ep->rset);
  FD_ZERO(&ep->wset);
  FD_ZERO(&ep->eset);
  epoll_spinlock_init(&ep->lock);
  for (int i = 0; i < EPOLL_MAX_EVENTS; i++) ep->udata[i]._nouse = 1; // 表示未使用fd.
  struct epoll_event ev; ev.data.ptr = 0; ev.events = EPOLLIN; ev._nouse =1;
  uepoll_add(ep, ep->pipes[0], &ev);
  return (HANDLE)ep;
}

int epoll_wait(HANDLE efd, struct epoll_event *events, int maxevents, int timeout)
{
  errno = 0;

  if (efd <= 0) {
    errno = EBADF; // epfd is not a valid file descriptor.
    return EPOLL_INVALID;
  }

  if (!events) {
    errno = EFAULT;
    return EPOLL_INVALID;
  }

  if (maxevents <= 0) {
    errno = EINVAL;
    return EPOLL_INVALID;
  }

  if (maxevents > EPOLL_MAX_EVENTS)
    maxevents = EPOLL_MAX_EVENTS;

  int r;
  struct epoll_t* ep = (struct epoll_t*)efd;
  while (1)
  {
    struct timeval ts = {0, 0}; int64_t wait_time;
    struct timeval *tp = uepoll_timeout2timeval(timeout, &ts, &wait_time);
    // printf("struct timeval ts {%zd, %zd}\n", ts.tv_sec, ts.tv_usec);

    epoll_spinlock_lock(&ep->lock);
    epoll_receive(ep);
    fd_set rset, wset, eset;
    int nfds = ep->nfds;
    FD_COPY(&ep->rset, &rset); FD_COPY(&ep->wset, &wset); FD_COPY(&ep->eset, &eset);
    epoll_spinlock_unlock(&ep->lock);

    int nevents = 0; struct epoll_event* ev;
    r = select(nfds, &rset, &wset, &eset, tp);

    /* --- Merge closing check with event processing under one lock ---
     * This eliminates the TOCTOU window where epoll_close could free
     * ep between the two previous lock acquisitions. */
    epoll_spinlock_lock(&ep->lock);

    /* EINTR retry with timeout decay */
    if (r < 0) {
      if (errno == EINTR) {
        epoll_spinlock_unlock(&ep->lock);
        if (timeout > 0) {
          timeout -= (epoll_now_ms() - wait_time);
          if (timeout < 0) timeout = 0;
        }
        continue;
      }
      epoll_spinlock_unlock(&ep->lock);
      break;
    }

    if (ep->closing) { epoll_spinlock_unlock(&ep->lock); return EPOLL_INVALID; }

    if (r > 0)
    {
      if (FD_ISSET(ep->pipes[0], &rset)) {
        if (timeout > 0) {
          timeout -= (epoll_now_ms() - wait_time);
          if (timeout < 0) timeout = 0;
        }
        epoll_spinlock_unlock(&ep->lock);
        continue;
      }

      for (SOCKET fd = 0; fd < nfds; fd++)
      {
        if (uepoll_has_fd(ep, fd))
        {
          ev = &events[nevents];
          ev->events = EPOLLEMPTY;

          if (FD_ISSET(fd, &rset)) {
            ev->events |= EPOLLIN; r--;
          }

          if (FD_ISSET(fd, &wset)) {
            ev->events |= EPOLLOUT; r--;
          }

          if (FD_ISSET(fd, &eset)) {
            /* select(2) reports both OOB data and error conditions
             * through the same error fd_set.  Check what the user
             * registered to decide how to flag the event. */
            if (ep->udata[fd].events & EPOLLPRI)
              ev->events |= EPOLLPRI;
            if (ep->udata[fd].events & EPOLLERR)
              ev->events |= EPOLLERR;
            r--;
          }

          if (ev->events) {
            ev->data.ptr = ep->udata[fd].data.ptr;
            if (uepoll_has_oneshot(ep->udata[fd].events)) {
              FD_CLR(fd, &ep->rset);
              FD_CLR(fd, &ep->wset);
              FD_CLR(fd, &ep->eset);
              ep->udata[fd]._nouse = 1;
              ep->udata[fd].events = EPOLLEMPTY;
              ep->udata[fd].data.ptr = NULL;
            }
            nevents = nevents + 1;
            if (nevents == maxevents)
              break;
            if (r <= 0)
              break;
          }

        }
      }
      r = nevents;
    }
    epoll_spinlock_unlock(&ep->lock);
    break;
  }
  // Done.
  return r;
}
