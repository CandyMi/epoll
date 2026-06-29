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
**  Pre-allocates a heap kevent buffer (EPOLL_MAX_EVENTS) to avoid alloca.
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
// alloca removed — kqevents now lives on the heap

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

#ifndef EPOLL_MAX_EVENTS
  #define EPOLL_MAX_EVENTS 4096
#endif

#define epoll_malloc(sz)  epoll_realloc(NULL, sz)
#define epoll_free(ptr)   (void)epoll_realloc(ptr, 0)
#define epoll_op_in(op)   ((op) == ((op) & (EPOLL_CTL_ADD | EPOLL_CTL_MOD | EPOLL_CTL_DEL)))

struct epoll_t
{
  int efd;
  epoll_lock_t lock;
  volatile int closing;         /* 1 = epoll_close in progress */
  struct kevent *kqevents;      /* heap-allocated kevent buffer, sized EPOLL_MAX_EVENTS */
};

static epoll_realloc_t epoll_realloc = realloc;
void epoll_allocator(epoll_realloc_t alloc)
{
  if (!alloc) return;
  epoll_realloc = alloc;
}

static inline int kepoll_ensure_kqevents(struct epoll_t *ep) {
  if (!ep->kqevents) {
    ep->kqevents = (struct kevent*)epoll_malloc(sizeof(struct kevent) * EPOLL_MAX_EVENTS);
    if (!ep->kqevents) return -1;
  }
  return 0;
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

  epoll_free(ep->kqevents);
  ep->kqevents = NULL;
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
  ep->kqevents = NULL;
  epoll_spinlock_init(&ep->lock);
  /* pre-allocate kevent buffer to avoid alloca on every epoll_wait */
  if (kepoll_ensure_kqevents(ep) != 0) {
    errno = ENOMEM;
    close(efd);
    epoll_free(ep);
    return -1;
  }
  return (HANDLE)ep;
}

static inline
int _kepoll_register(struct epoll_t *ep, SOCKET fd, struct epoll_event *event, bool first)
{
  if (!event) {
    errno = EFAULT;
    return EPOLL_INVALID;
  }
  epoll_spinlock_lock(&ep->lock);
  int efd = ep->efd;
  errno = 0; struct kevent ev[3];
  /* 一次性事件 */
  int events = event->events; void *udata = event->data.ptr;
  /* 虽然添加, 但是可以不启用 */
  int filter = 0; int flags = 0; int nevent = 0; 
  // kqueue 的 EV_ADD 是 upsert 语义：不存在则添加，存在则更新。
  // ADD 和 MOD 都统一使用 EV_ADD，确保 MOD 能正确更新 udata / flags。
  uint32_t add = EV_ADD;
  uint32_t exflags = (events & EPOLLONESHOT) ? EV_ONESHOT : 0;
  if (events & EPOLLET) // 模拟`ET`模式
    exflags |= EV_CLEAR;
  // printf("events & EPOLLET = 0x%08x, flags = 0x%04x\n", events & EPOLLET, exflags);
  /* 读事件 — 只有读相关事件（含 EPOLLERR/EPOLLHUP/EPOLLRDHUP）才启用 EVFILT_READ。
   * 注意这不同于 Linux epoll 始终监听 EPOLLERR/EPOLLHUP 的语义；但 kqueue 上
   * 连接断开也会通过 EVFILT_WRITE 的 EV_EOF/EV_ERROR 上报，所以纯写场景不漏。 */
  if (events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    filter = EVFILT_READ; flags = add | EV_ENABLE | exflags;
    EV_SET(&ev[nevent++], fd, filter, flags, 0, 0, udata);
  } else {
    filter = EVFILT_READ; flags = add | EV_DISABLE;
    EV_SET(&ev[nevent++], fd, filter, flags, 0, 0, udata);
  }
  /* 写事件 */
  if (events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) {
    filter = EVFILT_WRITE; flags = add | EV_ENABLE | exflags;
    EV_SET(&ev[nevent++], fd, filter, flags, 0, 0, udata);
  } else {
    filter = EVFILT_WRITE; flags = add | EV_DISABLE;
    EV_SET(&ev[nevent++], fd, filter, flags, 0, 0, udata);
  }
  /* 异常/带外事件 → EVFILT_EXCEPT (NOTE_OOB) — 与 READ/WRITE 合入同一次 kevent，
   * 减少系统调用。pipe 等 fd 不支持 EVFILT_EXCEPT（错误码因平台而异），
   * 回退去掉 EXCEPT 条目重试。 */
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
  /* 一次提交所有事件 — 最多 3 个 (READ + WRITE + EXCEPT) */
  int r = kevent(efd, ev, nevent, NULL, 0, NULL);
#if defined(EVFILT_EXCEPT) && defined(NOTE_OOB)
  if (r < 0 && has_except) {
    /* pipe 不支持 EVFILT_EXCEPT — 去掉最后一条重试 */
    r = kevent(efd, ev, nevent - 1, NULL, 0, NULL);
  }
#endif
  epoll_spinlock_unlock(&ep->lock);
  return r;
}

static inline
int kepoll_add(struct epoll_t *ep, SOCKET fd, struct epoll_event *event)
{
  return _kepoll_register(ep, fd, event, true);
}

static inline
int kepoll_mod(struct epoll_t *ep, SOCKET fd, struct epoll_event *event)
{
  return _kepoll_register(ep, fd, event, false);
}

static inline
int kepoll_del(struct epoll_t *ep, SOCKET fd, bool deleted)
{
  epoll_spinlock_lock(&ep->lock);
  int efd = ep->efd;
  int action = deleted ? EV_DELETE : EV_DISABLE;
  /* 可以一次系统调用可以删除/关闭此`fd`的所有事件 */
  struct kevent ev[3]; int nevents = 0;
  EV_SET(&ev[nevents++], fd, EVFILT_READ, action, 0, 0, NULL);
  EV_SET(&ev[nevents++], fd, EVFILT_WRITE, action, 0, 0, NULL);
  (void)kevent(efd, ev, nevents, NULL, 0, NULL);
  // int r = kevent(efd, ev, nevents, NULL, 0, NULL);
  // if (r) perror("kepoll_del");
  errno = 0;
  epoll_spinlock_unlock(&ep->lock);
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

  /* Reject operations on a closing epoll instance */
  { struct epoll_t *__ep = (struct epoll_t *)efd;
    epoll_spinlock_lock(&__ep->lock);
    int __closing = __ep->closing;
    epoll_spinlock_unlock(&__ep->lock);
    if (__closing) { errno = EBADF; return EPOLL_INVALID; }
  }

  switch (op)
  {
    case EPOLL_CTL_ADD: return kepoll_add((struct epoll_t *)efd, fd, event);
    case EPOLL_CTL_DEL: return kepoll_del((struct epoll_t *)efd, fd, true);
    case EPOLL_CTL_MOD: return kepoll_mod((struct epoll_t *)efd, fd, event);
  }
  errno = EINVAL;
  // `op`必须是EPOLL_CTL_ADD, EPOLL_CTL_DEL或EPOLL_CTL_MOD.
  return EPOLL_INVALID;  
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

  /* Capture local copies under lock — safe from concurrent epoll_close */
  epoll_spinlock_lock(&ep->lock);
  if (ep->closing) {
    epoll_spinlock_unlock(&ep->lock);
    errno = EBADF;
    return EPOLL_INVALID;
  }
  int local_efd = ep->efd;
  struct kevent *kqevents = ep->kqevents;
  epoll_spinlock_unlock(&ep->lock);

  struct epoll_event *ev;
  int nevents = kevent(local_efd, NULL, 0, kqevents, maxevents, tsp);

  /* Check for concurrent close — kevent may have been interrupted */
  epoll_spinlock_lock(&ep->lock);
  if (ep->closing) {
    epoll_spinlock_unlock(&ep->lock);
    errno = EBADF;
    return EPOLL_INVALID;
  }
  epoll_spinlock_unlock(&ep->lock);
  // printf("nevents = %d\n", nevents);
  if (nevents > 0)
  {
    /**
     * 这里要做`2`件事来模拟`epoll`行为:
     * 1. 合并相同`fd`的事件.
     * 2. 实现单`fd`的`ONESHOT`删除.
     */
    uintptr_t before_fd = (uintptr_t)-1; int nkqevents = 0;
    for (int i = 0; i < nevents; i++)
    {
      /* 标记上事件的fd, 方便融合事件. 相同fd的事件总是在旁边 */
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
        /* ONESHOT 事件先输出到 events[]，再清理内部状态 */
        if (flags & EV_ERROR) ev->events |= EPOLLERR;
        if (flags & EV_EOF)   ev->events |= EPOLLHUP;
        if (kqevents[i].filter == EVFILT_READ) ev->events |= EPOLLIN;
        if (kqevents[i].filter == EVFILT_WRITE) ev->events |= EPOLLOUT;
        ev->data.ptr = kqevents[i].udata;
        kepoll_del(ep, kqevents[i].ident, false);
        continue;
      } else if (flags & EV_ERROR || flags & EV_EOF) {
        // printf("EV_ERROR | EV_EOF %ld, 0x%08x\n", kqevents[i].ident, flags);
        kepoll_del(ep, kqevents[i].ident, false);
        if (flags & EV_ERROR) ev->events |= EPOLLERR;
        if (flags & EV_EOF)   ev->events |= EPOLLHUP;
        if (kqevents[i].filter == EVFILT_READ) ev->events |= EPOLLIN;
        if (kqevents[i].filter == EVFILT_WRITE) ev->events |= EPOLLOUT;
        ev->data.ptr = kqevents[i].udata;
        continue;
      }

      /* 判断 kqueue 事件 */
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
  return nevents;
}