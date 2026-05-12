#include "uepoll.h"

// #include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/event.h>

// #define EPOLL_NO_THREADS 1
#if defined(EPOLL_NO_THREADS)
  /* 如果假设不会有多线程的情况, 则不需要任何逻辑保证线程安全 */
  typedef int epoll_lock_t;
  #define epoll_spinlock_init(lock)       ((void)lock)
  #define epoll_spinlock_lock(lock)       ((void)lock)
  #define epoll_spinlock_unlock(lock)     ((void)lock)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  #include <stdatomic.h>
  typedef atomic_flag epoll_lock_t;
  #define epoll_spinlock_init(lock)       atomic_flag_clear((lock))
  #define epoll_spinlock_lock(lock)       do {} while (atomic_flag_test_and_set((lock)) == 1)
  #define epoll_spinlock_unlock(lock)     atomic_flag_clear((lock))
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
#define EPOLL_MAX_EVENTS 1024

#define epoll_malloc(sz)  epoll_realloc(NULL, sz)
#define epoll_free(ptr)   (void)epoll_realloc(ptr, 0)
#define epoll_op_in(op)   ((op) == ((op) & (EPOLL_CTL_ADD | EPOLL_CTL_MOD | EPOLL_CTL_DEL)))

struct epoll_t
{
  int efd;
  epoll_lock_t lock;
};

static epoll_realloc_t epoll_realloc = realloc;
void epoll_allocator(epoll_realloc_t alloc)
{
  epoll_realloc = alloc;
}

int epoll_close(HANDLE efd)
{
  if (efd <= 0)
    return -1;
  struct epoll_t* ep = (struct epoll_t*)efd;
  if (ep->efd <= 0)
    return -1;
  close(ep->efd);
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
  errno = 0;
  int efd = kqueue();
  if (efd == -1)
    return -1;
  if (flags & EPOLL_CLOEXEC)
    fcntl(efd, F_SETFD, FD_CLOEXEC);
  struct epoll_t *ep = epoll_malloc(sizeof(struct epoll_t));
  ep->efd = efd; epoll_spinlock_init(&ep->lock);
  return (HANDLE)ep;
}

static inline
int _kepoll_register(struct epoll_t *ep, SOCKET fd, struct epoll_event *event, bool first)
{
  epoll_spinlock_lock(&ep->lock);
  int efd = ep->efd;
  errno = 0; struct kevent ev[3];
  /* 一次性事件 */
  int events = event->events; void *udata = event->data.ptr;
  /* 虽然添加, 但是可以不启用 */
  int filter = 0; int flags = 0; int nevent = 0; 
  uint32_t add = first ? EV_ADD : 0; // 首次必须是添加模式
  uint32_t exflags = (events & EPOLLONESHOT) ? EV_ONESHOT : 0;
  if (events & EPOLLET) // 模拟`ET`模式
    exflags |= EV_CLEAR;
  // printf("events & EPOLLET = 0x%08x, flags = 0x%04x\n", events & EPOLLET, exflags);
  /* 读事件 */
  if (events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND)) {
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
  /* 注册事件 */
  int r = kevent(efd, ev, nevent, NULL, 0, NULL);
  // if (r) perror("kepoll_add");
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
  int r;
  int action = deleted ? EV_DELETE : EV_DISABLE;
  /* 可以一次系统调用可以删除/关闭此`fd`的所有事件 */
  struct kevent ev[3]; int nevents = 0;
  EV_SET(&ev[nevents++], fd, EVFILT_READ, action, 0, 0, NULL);
  EV_SET(&ev[nevents++], fd, EVFILT_WRITE, action, 0, 0, NULL);
  r = kevent(efd, ev, nevents, NULL, 0, NULL);
  // if (r) perror("kepoll_del");
  errno = 0;
  epoll_spinlock_unlock(&ep->lock);
  return 0;
}

int epoll_ctl(HANDLE efd, int op, SOCKET fd, struct epoll_event *event)
{
  errno = 0;
  if (efd <= 0) {
    errno = EBADF;
    return EPOLL_INVALID;
  }

  if (!(epoll_op_in(op))) {
    errno = EINVAL;
    return EPOLL_INVALID;
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

  if (efd < 0) {
    errno = EBADF; // 指针不会为负数.
    return EPOLL_INVALID;
  }
  struct epoll_t *ep = (struct epoll_t *)efd;
  // efd = ep->efd;
  if (!events) {
    errno = EFAULT;
    return EPOLL_INVALID;
  }

  if (maxevents <= 0) {
    errno = EINVAL;
    return EPOLL_INVALID;
  }
  if (maxevents > EPOLL_MAX_EVENTS) maxevents = EPOLL_MAX_EVENTS;

  /* 兼容两者的时间行为 */
  struct timespec ts; struct timespec *tsp = NULL;
  if (timeout >= 0) {
    tsp = &ts;
    ts.tv_nsec = (timeout % 1000) * 1000000;
    ts.tv_sec  = (timeout - timeout % 1000) / 1000;
    // printf("struct timespec = {%ld, %ld}\n", ts.tv_sec, ts.tv_nsec);
  }

  struct kevent kqevents[maxevents]; struct epoll_event *ev;
  int nevents = kevent(ep->efd, NULL, 0, kqevents, maxevents, tsp);
  // printf("nevents = %d\n", nevents);
  if (nevents > 0)
  {
    /**
     * 这里要做`2`件事来模拟`epoll`行为:
     * 1. 合并相同`fd`的事件.
     * 2. 实现单`fd`的`ONESHOT`删除.
     */
    uint32_t before_fd = -1; int pos = 0; int nkqevents = 0;
    for (int i = 0; i < nevents; i++)
    {
      uint16_t flags = kqevents[i].flags;
      if (flags & EV_ONESHOT) {
        kepoll_del(ep, kqevents[i].ident, false);
      } else if (flags & EV_ERROR || flags & EV_EOF) {
        // printf("EV_ERROR | EV_EOF %ld, 0x%08x\n", kqevents[i].ident, flags);
        kepoll_del(ep, kqevents[i].ident, false);
        ev = &events[i];
        ev->events = EPOLLERR;
        if (kqevents[i].filter == EVFILT_READ) ev->events |= EPOLLIN;
        if (kqevents[i].filter == EVFILT_READ) ev->events |= EPOLLOUT;
        ev->data.ptr = kqevents[i].udata;
        continue;
      }
      // printf("kqevents[%d] = {.fd = %ld, .filter = %d, .flags = 0x%08x, .fflags = 0x%08x}\n", i, kqevents[i].ident, kqevents[i].filter, kqevents[i].flags, kqevents[i].fflags);
      /* 试图合并事件... */
      bool eqfd = before_fd == kqevents[i].ident;
      if (!eqfd) {
        /* 标记上事件的fd, 方便融合事件. 相同fd的事件总是在旁边 */
        before_fd = kqevents[i].ident;
        ev = &events[i];
        ev->events = EPOLLEMPTY;
        ev->data.ptr = kqevents[i].udata;
        nkqevents++; pos = i;
      } else {
        ev = &events[pos];
      }

      /* 判断 kqueue 事件 */
      switch (kqevents[i].filter)
      {
        case EVFILT_READ:
        {
          ev->events |= EPOLLIN;
          break;
        }
        case EVFILT_WRITE:
        {
          ev->events |= EPOLLOUT;
          break;
        }
      }

    }
    nevents = nkqevents;
  }
  return nevents;
}