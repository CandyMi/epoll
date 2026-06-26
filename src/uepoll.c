#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif

#include "uepoll.h"

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdbool.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>

#ifndef FD_COPY
  #define FD_COPY(f, t)  memcpy(t, f, sizeof(*f));
#endif

// #define EPOLL_NO_THREADS 1
#if defined(EPOLL_NO_THREADS)
  /* 如果假设不会有多线程的情况, 则不需要任何逻辑保证线程安全 */
  #define uepoll_use_spinlock -1
  typedef int epoll_lock_t;
  #define epoll_spinlock_init(lock)       ((void)lock)
  #define epoll_spinlock_lock(lock)       ((void)lock)
  #define epoll_spinlock_unlock(lock)     ((void)lock)
#elif defined(EPOLL_USE_ATOMIC) || \
  (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__))
  #define uepoll_use_spinlock 1
  #include <stdatomic.h>
  typedef atomic_flag epoll_lock_t;
  #define epoll_spinlock_init(lock)       atomic_flag_clear((lock))
  #define epoll_spinlock_lock(lock)       do {} while (atomic_flag_test_and_set((lock)) == 1)
  #define epoll_spinlock_unlock(lock)     atomic_flag_clear((lock))
#elif  defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1) || \
       defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_2) || \
       defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4) || \
       defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8)
  #define uepoll_use_spinlock 2
  typedef int epoll_lock_t;
  #define epoll_spinlock_init(lock)       __sync_lock_test_and_set((lock), 0)
  #define epoll_spinlock_lock(lock)       do { __sync_synchronize(); } while (__sync_lock_test_and_set((lock), 1) == 1)
  #define epoll_spinlock_unlock(lock)     __sync_lock_release((lock))
#else
  #error "Hey! Why doesn't your compiler support atomic operations yet?"
#endif

#define EPOLL_MAX_EVENTS FD_SETSIZE

#define EPOLLEMPTY  (0) // 空事件

#define EPOLL_SUCCESS  (0)
#define EPOLL_INVALID  (-1)

#define uepoll_has_events(e)    (e & (EPOLLIN | EPOLLOUT | EPOLLERR))
#define uepoll_has_oneshot(e)   (e & EPOLLONESHOT)

#define epoll_nonexec(fd)  fcntl(fd, F_SETFD, FD_CLOEXEC | fcntl(fd, F_GETFD))
#define epoll_nonblock(fd) fcntl(fd, F_SETFL, O_NONBLOCK | fcntl(fd, F_GETFL))

#define epoll_notice(ep)  { ep->edited = true; write((ep)->pipes[1], "x", 1); }


#define epoll_malloc(sz)  epoll_realloc(NULL, sz)
#define epoll_free(ptr)   (void)epoll_realloc(ptr, 0)

struct epoll_t
{
  bool edited;
  int nfds;
  int pipes[2];
  epoll_lock_t lock;
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
int64_t uepoll_now()
{
  /**
   * 不同时间源的调用性能差异很大, 因此尽量使用最快的时间源获取时间.
   */
#if defined(CLOCK_MONOTONIC_COARSE)
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
  return now.tv_sec * 1000 + now.tv_nsec / 1000000;
#elif defined(CLOCK_REALTIME_COARSE)
  struct timespec now;
  clock_gettime(CLOCK_REALTIME_COARSE, &now);
  return now.tv_sec * 1000 + now.tv_nsec / 1000000;
#else
  struct timeval now; gettimeofday(&now, NULL);
  return now.tv_sec * 1000 + now.tv_usec / 1000;
#endif
}

static inline
struct timeval* uepoll_timeout2timeval(int timeout, struct timeval* tv, int64_t *now)
{
  if (timeout >= 0) {
    tv->tv_sec = (timeout - timeout % 1000) / 1000;
    tv->tv_usec = (timeout % 1000) * 1000;
    if (now) *now = uepoll_now();
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

static inline
int uepoll_del(struct epoll_t* ep, SOCKET fd)
{
  epoll_spinlock_lock(&ep->lock);
  if (!uepoll_has_fd(ep, fd)) {
    errno = ENOENT; /* 删除`fd`之前没有注册过. */
    epoll_spinlock_unlock(&ep->lock);
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
  epoll_spinlock_unlock(&ep->lock);
  return EPOLL_SUCCESS;
}

static inline
int uepoll_add(struct epoll_t* ep, SOCKET fd, struct epoll_event* ev)
{
  epoll_spinlock_lock(&ep->lock);
  if (uepoll_has_fd(ep, fd)) {
    errno = EEXIST; /* 不能重复调用`EPOLL_CTL_ADD`注册同一个`fd`. */
    epoll_spinlock_unlock(&ep->lock);
    return EPOLL_INVALID;
  }

  ep->udata[fd]._nouse = 0; // 标记`fd`已注册.
  ep->udata[fd].events = EPOLLEMPTY; // 清空`events`.
  ep->udata[fd].data.ptr = NULL; // 清空`data`.

  if (ev->events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND)) {
    FD_SET(fd, &ep->rset);
    ep->udata[fd].events |= EPOLLIN;
  }

  if (ev->events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) {
    FD_SET(fd, &ep->wset);
    ep->udata[fd].events |= EPOLLOUT;
  }

  if (ev->events & EPOLLERR) {
    FD_SET(fd, &ep->eset);
    ep->udata[fd].events |= EPOLLERR;
  }

  if (uepoll_has_oneshot(ev->events)) {
    ep->udata[fd].events |= EPOLLONESHOT;
  }

  if (uepoll_has_events(ep->udata[fd].events)) {
    ep->udata[fd].data.ptr = ev->data.ptr;
    uepoll_append_nfds(ep, fd);
    epoll_notice(ep);
  }

  epoll_spinlock_unlock(&ep->lock);
  return EPOLL_SUCCESS;
}

static inline 
int uepoll_mod(struct epoll_t* ep, int fd, struct epoll_event *ev)
{
  epoll_spinlock_lock(&ep->lock);
  if (!uepoll_has_fd(ep, fd)) {
    errno = ENOENT; /* 修改`fd`之前没注册过. */
    epoll_spinlock_unlock(&ep->lock);
    return EPOLL_INVALID;
  }

  ep->udata[fd].events = EPOLLEMPTY; // 清空`events`.
  ep->udata[fd].data.ptr = NULL; // 清空`data`.

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

  if (ev->events & EPOLLERR) {
    FD_SET(fd, &ep->eset);
    ep->udata[fd].events |= EPOLLERR;
  } else {
    FD_CLR(fd, &ep->eset);
  }

  if (uepoll_has_oneshot(ev->events)) {
    ep->udata[fd].events |= EPOLLONESHOT;
  }

  ep->udata[fd].data.ptr = ev->data.ptr;
  epoll_notice(ep);
  uepoll_append_nfds(ep, fd);
  epoll_spinlock_unlock(&ep->lock);
  return EPOLL_SUCCESS;
}

int epoll_ctl(HANDLE efd, int op, SOCKET fd, struct epoll_event *event)
{
  errno = 0;
  if (efd <= 0) {
    errno = EBADF; // 指针不会为负数.
    return EPOLL_INVALID;
  }

  if (fd < 0 || fd >= EPOLL_MAX_EVENTS) {
    errno = ENOSPC; // `fd`必须在0~FD_SETSIZE之间.
    return EPOLL_INVALID;
  }

  if (!uepoll_can_poll(fd)) {
    errno = EPERM; // 不支持的`fd`类型.
    return EPOLL_INVALID;
  }

  switch (op)
  {
    case EPOLL_CTL_ADD: return uepoll_add((struct epoll_t*)efd, fd, event);
    case EPOLL_CTL_DEL: return uepoll_del((struct epoll_t*)efd, fd);
    case EPOLL_CTL_MOD: return uepoll_mod((struct epoll_t*)efd, fd, event);
  }
  errno = EINVAL;
  // `op`必须是EPOLL_CTL_ADD, EPOLL_CTL_DEL或EPOLL_CTL_MOD.
  return EPOLL_INVALID;
}

int epoll_close(HANDLE efd)
{
  if (efd <= 0) {
    errno = EBADF; // 指针不会为负数.
    return EPOLL_INVALID;
  }

  struct epoll_t* ep = (struct epoll_t*)efd;
  close(ep->pipes[0]); ep->pipes[0] = -1;
  close(ep->pipes[1]); ep->pipes[1] = -1;
  epoll_free(ep);
  return EPOLL_SUCCESS;
}

HANDLE epoll_create(int size)
{
  (void)size;
  return epoll_create1(0);
}

HANDLE epoll_create1(int flags)
{
  errno = 0;
  struct epoll_t *ep = (struct epoll_t *)epoll_malloc(sizeof(struct epoll_t));
  if (!ep) {
    errno = ENOMEM;
    return -1;
  }
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
    errno = EBADF; // 指针不会为负数.
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

  struct timeval ts = {0, 0}; int64_t wait_time;
  struct timeval *tp = uepoll_timeout2timeval(timeout, &ts, &wait_time);
  // printf("struct timeval ts {%zd, %zd}\n", ts.tv_sec, ts.tv_usec);

  struct epoll_t* ep = (struct epoll_t*)efd;
  epoll_spinlock_lock(&ep->lock);
  epoll_receive(ep); /* 每次监听前必须重置状态（多一次系统调用） */
  fd_set rset, wset, eset; /* 使用复制拷贝来解决多线程竞争的问题 */
  int nfds = ep->nfds;
  FD_COPY(&ep->rset, &rset); FD_COPY(&ep->wset, &wset); FD_COPY(&ep->eset, &eset);
  epoll_spinlock_unlock(&ep->lock);

  int nevents = 0; struct epoll_event* ev;
  int r = select(nfds, &rset, &wset, &eset, tp);
  if (r > 0)
  { // select 返回的是事件数量, 不是文件描述符的数量.
    if (FD_ISSET(ep->pipes[0], &rset)) {
      /**
       * 这里需要处理以下情况：
       * 1. 其它线程可能修改了`3`个集合内的`fd`，因此需要重新监听已注册的`fd`.
       * 2. 如果`timeout > 0`则必须计算`select`调用过去了多久时间.
       */
      if (timeout > 0) {
        wait_time = uepoll_now() - wait_time;
        timeout = wait_time >= timeout ? 0 : timeout - wait_time;
      }
      return epoll_wait(efd, events, maxevents, timeout);
    }
    // printf("r = %d, nfds = %d\n", r, nfds);
    /* 检查`fd`是否被设置 */
    epoll_spinlock_lock(&ep->lock);
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
          ev->events |= EPOLLERR; r--;
        }

        if (ev->events) {
          ev->data.ptr = ep->udata[fd].data.ptr;
          /* 如果携带`EPOLLONESHOT`属性, 则每次都需要重新修改事件 */
          if (uepoll_has_oneshot(ep->udata[fd].events)) {
            FD_CLR(fd, &ep->rset);
            FD_CLR(fd, &ep->wset);
            FD_CLR(fd, &ep->eset);
            ep->udata[fd]._nouse = 0;
            ep->udata[fd].events = EPOLLEMPTY;
            ep->udata[fd].data.ptr = NULL;
          }
          nevents = nevents + 1;
          /* 最多只能返回`maxevents`个事件. */
          if (nevents == maxevents)
            break;
          /* 所有事件已经处理完毕. */
          if (r <= 0)
            break;
        }

      }
    }
    epoll_spinlock_unlock(&ep->lock);
    r = nevents;
  }
  // Done.
  return r;
}
