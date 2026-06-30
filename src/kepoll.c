/*
**  LICENSE: MIT
**  Author: CandyMi[https://github.com/candymi]
**
**  kepoll.c — kqueue-based epoll emulation for BSD / Darwin (macOS).
**
**  Translates the epoll API to kqueue / kevent:
**    • EPOLLIN / EPOLLOUT → EVFILT_READ / EVFILT_WRITE
**    • EPOLLET → EV_CLEAR
**    • EPOLLONESHOT → EV_ONESHOT
**    • EPOLLPRI → EVFILT_EXCEPT | NOTE_OOB (macOS only)
**
**  Merges adjacent same-fd READ+WRITE events into a single epoll_event.
**  Uses a per-call kevent buffer (stack ≤ 256, heap otherwise) so multiple
**  threads can call epoll_wait concurrently on the same epoll instance.
**
**  Selected on BSD / Darwin systems.
*/

#include "uepoll.h"

// #include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/event.h>

/* Per-call kevent buffer: stack for ≤ 256 events, heap fallback for larger.
 * sizeof(struct kevent) ≈ 32 bytes on 64-bit, so stack cost ≈ 8 KB. */
#define KE_STACK_NEVENTS 256

// #define EPOLL_NO_THREADS 1
#if defined(EPOLL_NO_THREADS)
  /* 如果假设不会有多线程的情况, 则不需要任何逻辑保证线程安全 */
  typedef int epoll_lock_t;
  #define epoll_spinlock_init(lock)       ((void)lock)
  #define epoll_spinlock_lock(lock)       ((void)lock)
  #define epoll_spinlock_unlock(lock)     ((void)lock)
#elif defined(EPOLL_USE_ATOMIC) || \
  (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__))
  #include <stdatomic.h>
  typedef atomic_flag epoll_lock_t;
  #define epoll_spinlock_init(lock)       atomic_flag_clear_explicit((lock), memory_order_release)
  #define epoll_spinlock_lock(lock)       do {} while (atomic_flag_test_and_set_explicit((lock), memory_order_acquire))
  #define epoll_spinlock_unlock(lock)     atomic_flag_clear_explicit((lock), memory_order_release)
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1) \
   || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_2) \
   || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4) \
   || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8)
  typedef int epoll_lock_t;
  #define epoll_spinlock_init(lock)       __sync_lock_test_and_set((lock), 0)
  #define epoll_spinlock_lock(lock)       do { __sync_synchronize(); } while (__sync_lock_test_and_set((lock), 1) == 1)
  #define epoll_spinlock_unlock(lock)     __sync_lock_release((lock))
#else
  #error "Hey! Why doesn't your compiler support atomic operations yet?"
#endif

#define EPOLL_INVALID (-1)
#define EPOLLEMPTY (0)

/* Per-wait max events cap (power of two).
 * epoll_wait returns at most this many events per call. */
#ifndef EPOLL_MAX_EVENTS
  #define EPOLL_MAX_EVENTS 65536
#endif

/* Initial capacity for the shared working buffer (if EPOLL_USE_SHARED_WORKBUF=1).
 * Grows by 2^n from here up to EPOLL_MAX_EVENTS. */
#ifndef EPOLL_INITIAL_CAP
  #define EPOLL_INITIAL_CAP 512
#endif

/* Round n up to the next power of two. */
static inline size_t round_up_pow2(size_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4;
    n |= n >> 8; n |= n >> 16;
#if SIZE_MAX > UINT32_MAX
    n |= n >> 32;
#endif
    return n + 1;
}

#define epoll_malloc(sz)  epoll_realloc(NULL, sz)
#define epoll_free(ptr)   (void)epoll_realloc(ptr, 0)
#define epoll_op_in(op)   ((op) == ((op) & (EPOLL_CTL_ADD | EPOLL_CTL_MOD | EPOLL_CTL_DEL)))

struct epoll_t
{
  int efd;
  epoll_lock_t lock;
  volatile int closing;         /* 1 = epoll_close in progress */
#if EPOLL_USE_SHARED_WORKBUF
  struct kevent *kqevents;      /* shared kevent buffer (2^n heap, lazy) */
  size_t         kqcap;         /* current kqevents capacity */
#endif
};

static epoll_realloc_t epoll_realloc = realloc;
void epoll_allocator(epoll_realloc_t alloc)
{
  if (!alloc) return;
  epoll_realloc = alloc;
}

int epoll_close(HANDLE efd)
{
  if (efd <= 0) {
    errno = EBADF;
    return -1;
  }
  struct epoll_t* ep = (struct epoll_t*)efd;

  epoll_spinlock_lock(&ep->lock);
  if (ep->closing) {
    epoll_spinlock_unlock(&ep->lock);
    errno = EBADF;
    return -1;
  }
  ep->closing = 1;
  int saved_efd = ep->efd;
  ep->efd = -1;
  epoll_spinlock_unlock(&ep->lock);

  /* close the kqueue fd — this interrupts any kevent() in another thread */
  if (saved_efd > 0)
    close(saved_efd);

#if EPOLL_USE_SHARED_WORKBUF
  epoll_free(ep->kqevents);
#endif
  epoll_free(ep);
  return 0;
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
  int efd = kqueue();
  if (efd == -1)
    return -1;
  if (flags & EPOLL_CLOEXEC)
    fcntl(efd, F_SETFD, FD_CLOEXEC);
  struct epoll_t *ep = (struct epoll_t*)epoll_malloc(sizeof(struct epoll_t));
  if (!ep) {
    errno = ENOMEM;
    close(efd);
    return -1;
  }
  memset(ep, 0, sizeof(struct epoll_t));
  ep->efd = efd;
  epoll_spinlock_init(&ep->lock);
  return (HANDLE)ep;
}

/* Lock-held variants — assume ep->lock is held by epoll_ctl */
static inline
int _kepoll_register_locked(struct epoll_t *ep, SOCKET fd,
                             struct epoll_event *event, bool first)
{
  if (!event) {
    errno = EFAULT;
    return EPOLL_INVALID;
  }
  int efd = ep->efd;
  errno = 0; struct kevent ev[3];
  int events = event->events; void *udata = event->data.ptr;
  int filter = 0; int flags = 0; int nevent = 0;
  uint32_t add = EV_ADD;
  uint32_t exflags = (events & EPOLLONESHOT) ? EV_ONESHOT : 0;
  if (events & EPOLLET)
    exflags |= EV_CLEAR;
  if (events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    filter = EVFILT_READ; flags = add | EV_ENABLE | exflags;
    EV_SET(&ev[nevent++], fd, filter, flags, 0, 0, udata);
  } else {
    filter = EVFILT_READ; flags = add | EV_DISABLE;
    EV_SET(&ev[nevent++], fd, filter, flags, 0, 0, udata);
  }
  if (events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) {
    filter = EVFILT_WRITE; flags = add | EV_ENABLE | exflags;
    EV_SET(&ev[nevent++], fd, filter, flags, 0, 0, udata);
  } else {
    filter = EVFILT_WRITE; flags = add | EV_DISABLE;
    EV_SET(&ev[nevent++], fd, filter, flags, 0, 0, udata);
  }
#if defined(EVFILT_EXCEPT) && defined(NOTE_OOB)
  bool has_except = false;
  if (events & EPOLLPRI) {
    has_except = true;
    EV_SET(&ev[nevent++], fd, EVFILT_EXCEPT, EV_ADD | EV_ENABLE | exflags, NOTE_OOB, 0, udata);
  } else if (!first) {
    has_except = true;
    EV_SET(&ev[nevent++], fd, EVFILT_EXCEPT, EV_DELETE, 0, 0, 0);
  }
#endif
  int r = kevent(efd, ev, nevent, NULL, 0, NULL);
#if defined(EVFILT_EXCEPT) && defined(NOTE_OOB)
  if (r < 0 && has_except) {
    r = kevent(efd, ev, nevent - 1, NULL, 0, NULL);
  }
#endif
  return r;
}

static inline
int kepoll_add_locked(struct epoll_t *ep, SOCKET fd, struct epoll_event *event)
{
  return _kepoll_register_locked(ep, fd, event, true);
}

static inline
int kepoll_mod_locked(struct epoll_t *ep, SOCKET fd, struct epoll_event *event)
{
  return _kepoll_register_locked(ep, fd, event, false);
}

static inline
int kepoll_del_locked(struct epoll_t *ep, SOCKET fd, bool deleted)
{
  int efd = ep->efd;
  int action = deleted ? EV_DELETE : EV_DISABLE;
  struct kevent ev[3]; int nevents = 0;
  EV_SET(&ev[nevents++], fd, EVFILT_READ, action, 0, 0, NULL);
  EV_SET(&ev[nevents++], fd, EVFILT_WRITE, action, 0, 0, NULL);
  (void)kevent(efd, ev, nevents, NULL, 0, NULL);
  errno = 0;
  return 0;
}

int epoll_ctl(HANDLE efd, int op, SOCKET fd, struct epoll_event *event)
{
  errno = 0;

  if (efd <= 0 || fd < 0) {
    errno = EBADF;
    return EPOLL_INVALID;
  }

  if (op != EPOLL_CTL_DEL && !event) {
    errno = EFAULT;
    return EPOLL_INVALID;
  }

  if (!(epoll_op_in(op))) {
    errno = EINVAL;
    return EPOLL_INVALID;
  }

  /* Take lock FIRST — this eliminates the TOCTOU window where
   * epoll_close could free ep between a pre-check and dispatching
   * to the backend functions.  The locked variants assume the lock
   * is held and do NOT release it. */
  struct epoll_t *ep = (struct epoll_t *)efd;
  epoll_spinlock_lock(&ep->lock);

  int result;
  switch (op)
  {
    case EPOLL_CTL_ADD: result = kepoll_add_locked(ep, fd, event); break;
    case EPOLL_CTL_DEL: result = kepoll_del_locked(ep, fd, true); break;
    case EPOLL_CTL_MOD: result = kepoll_mod_locked(ep, fd, event); break;
    default: errno = EINVAL; result = EPOLL_INVALID; break;
  }

  epoll_spinlock_unlock(&ep->lock);
  return result;
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

  /* 兼容两者的时间行为 */
  struct timespec ts; struct timespec *tsp = NULL;
  if (timeout >= 0) {
    tsp = &ts;
    ts.tv_nsec = (timeout % 1000) * 1000000;
    ts.tv_sec  = (timeout - timeout % 1000) / 1000;
    // printf("struct timespec = {%ld, %ld}\n", ts.tv_sec, ts.tv_nsec);
  }

  struct epoll_t *ep = (struct epoll_t *)efd;

  /* ── Acquire kevent buffer ──
   *
   * Default (EPOLL_USE_SHARED_WORKBUF=0): per-call stack + heap.
   *   Stack for ≤ KE_STACK_NEVENTS (zero alloc), heap fallback for
   *   larger sets.  Safe for concurrent epoll_wait on the same handle.
   *
   * Shared (EPOLL_USE_SHARED_WORKBUF=1): buffer lives on epoll_t;
   *   realloc when maxevents exceeds current capacity.  NOT safe for
   *   concurrent epoll_wait on the same handle (undefined behavior). */
#if EPOLL_USE_SHARED_WORKBUF
  if (maxevents > (int)ep->kqcap) {
    size_t newcap = (size_t)maxevents;
    if (newcap > EPOLL_MAX_EVENTS) newcap = EPOLL_MAX_EVENTS;
    newcap = round_up_pow2(newcap);
    if (newcap < EPOLL_INITIAL_CAP) newcap = EPOLL_INITIAL_CAP;
    if (newcap > ep->kqcap) {
      struct kevent *p = (struct kevent *)epoll_realloc(ep->kqevents,
                              newcap * sizeof(struct kevent));
      if (!p) { errno = ENOMEM; return EPOLL_INVALID; }
      ep->kqevents = p;
      ep->kqcap    = newcap;
    }
  }
  struct kevent *kqevents = ep->kqevents;
#else
  struct kevent  kstack[KE_STACK_NEVENTS];
  struct kevent *kqevents = kstack;
  bool           heap     = false;
  if (maxevents > KE_STACK_NEVENTS) {
    kqevents = (struct kevent *)epoll_malloc((size_t)maxevents * sizeof(struct kevent));
    if (!kqevents) { errno = ENOMEM; return EPOLL_INVALID; }
    heap = true;
  }
#endif

  /* Capture local copies under lock — safe from concurrent epoll_close */
  while (1) {
    epoll_spinlock_lock(&ep->lock);
    if (ep->closing) {
      epoll_spinlock_unlock(&ep->lock);
#if !EPOLL_USE_SHARED_WORKBUF
      if (heap) epoll_free(kqevents);
#endif
      errno = EBADF;
      return EPOLL_INVALID;
    }
    int local_efd = ep->efd;
    epoll_spinlock_unlock(&ep->lock);

    struct epoll_event *ev;
    int nevents = kevent(local_efd, NULL, 0, kqevents, maxevents, tsp);

    /* Re-acquire lock to check closing and process events — merged
     * into one critical section so no epoll_close can free ep between
     * the check and the kepoll_del calls in the event loop. */
    epoll_spinlock_lock(&ep->lock);
    if (ep->closing) {
      epoll_spinlock_unlock(&ep->lock);
#if !EPOLL_USE_SHARED_WORKBUF
      if (heap) epoll_free(kqevents);
#endif
      errno = EBADF;
      return EPOLL_INVALID;
    }

    /* --- EINTR retry with timeout decay --- */
    if (nevents < 0) {
      if (errno == EINTR) {
        epoll_spinlock_unlock(&ep->lock);
        if (timeout > 0) {
          /* Approximate decay: kevent timeout is relative, just retry */
          timeout -= 100;
          if (timeout < 0) timeout = 0;
          tsp->tv_nsec = (timeout % 1000) * 1000000;
          tsp->tv_sec  = (timeout - timeout % 1000) / 1000;
        }
        continue;
      }
      epoll_spinlock_unlock(&ep->lock);
#if !EPOLL_USE_SHARED_WORKBUF
      if (heap) epoll_free(kqevents);
#endif
      return EPOLL_INVALID;
    }

    if (nevents > 0)
    {
      uintptr_t before_fd = (uintptr_t)-1; int nkqevents = 0;
      for (int i = 0; i < nevents; i++)
      {
        if (before_fd != kqevents[i].ident) {
          before_fd = kqevents[i].ident;
          ev = &events[nkqevents];
          ev->events = EPOLLEMPTY;
          ev->data.ptr = kqevents[i].udata;
          nkqevents++;
        } else {
          ev = &events[nkqevents - 1];
        }

        uint16_t flags = kqevents[i].flags;
        if (flags & EV_ONESHOT) {
          if (flags & EV_ERROR) ev->events |= EPOLLERR;
          if (flags & EV_EOF)   ev->events |= EPOLLHUP;
          if (kqevents[i].filter == EVFILT_READ) ev->events |= EPOLLIN;
          if (kqevents[i].filter == EVFILT_WRITE) ev->events |= EPOLLOUT;
          ev->data.ptr = kqevents[i].udata;
          kepoll_del_locked(ep, kqevents[i].ident, false);
          continue;
        } else if (flags & EV_ERROR || flags & EV_EOF) {
          kepoll_del_locked(ep, kqevents[i].ident, false);
          if (flags & EV_ERROR) ev->events |= EPOLLERR;
          if (flags & EV_EOF)   ev->events |= EPOLLHUP;
          if (kqevents[i].filter == EVFILT_READ) ev->events |= EPOLLIN;
          if (kqevents[i].filter == EVFILT_WRITE) ev->events |= EPOLLOUT;
          ev->data.ptr = kqevents[i].udata;
          continue;
        }

        switch (kqevents[i].filter)
        {
          case EVFILT_READ:   ev->events |= EPOLLIN; break;
          case EVFILT_WRITE:  ev->events |= EPOLLOUT; break;
#if defined (EVFILT_EXCEPT)
          case EVFILT_EXCEPT: ev->events |= EPOLLPRI; break;
#endif
        }
      }
      nevents = nkqevents;
    }
    epoll_spinlock_unlock(&ep->lock);

#if !EPOLL_USE_SHARED_WORKBUF
    if (heap) epoll_free(kqevents);
#endif
    return nevents;
  }
}