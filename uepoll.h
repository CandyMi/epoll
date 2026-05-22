/*
**  LICENSE: MIT
**  Author: CandyMi[https://github.com/candymi]
*/
#ifndef __UEPOLL_H__
#define __UEPOLL_H__

#ifndef UEPOLL_EXPORT
    #define UEPOLL_EXPORT __attribute__((visibility("default")))
#endif

#ifndef NO_NATIVE_EPOLL
    #define NO_NATIVE_EPOLL 1
#endif

#ifndef FD_SETSIZE
    #define FD_SETSIZE 1024
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef intptr_t HANDLE;
typedef int SOCKET;

enum EPOLL_EVENTS
{
    EPOLLIN = 0x001,
#define EPOLLIN EPOLLIN
    EPOLLPRI = 0x002,
#define EPOLLPRI EPOLLPRI
    EPOLLOUT = 0x004,
#define EPOLLOUT EPOLLOUT
    EPOLLRDNORM = 0x040,
#define EPOLLRDNORM EPOLLRDNORM
    EPOLLRDBAND = 0x080,
#define EPOLLRDBAND EPOLLRDBAND
    EPOLLWRNORM = 0x100,
#define EPOLLWRNORM EPOLLWRNORM
    EPOLLWRBAND = 0x200,
#define EPOLLWRBAND EPOLLWRBAND
    EPOLLMSG = 0x400,
#define EPOLLMSG EPOLLMSG
    EPOLLERR = 0x008,
#define EPOLLERR EPOLLERR
    EPOLLHUP = 0x010,
#define EPOLLHUP EPOLLHUP
    EPOLLRDHUP = 0x2000,
#define EPOLLRDHUP EPOLLRDHUP
    EPOLLEXCLUSIVE = 1u << 28,
#define EPOLLEXCLUSIVE EPOLLEXCLUSIVE
    EPOLLWAKEUP = 1u << 29,
#define EPOLLWAKEUP EPOLLWAKEUP
    EPOLLONESHOT = 1u << 30,
#define EPOLLONESHOT EPOLLONESHOT
    EPOLLET = 1u << 31
#define EPOLLET EPOLLET
};

/* Valid opcodes ( "op" parameter ) to issue to epoll_ctl().  */
#define EPOLL_CTL_ADD 1	/* Add a file descriptor to the interface.  */
#define EPOLL_CTL_MOD 2	/* Change file descriptor epoll_event structure.  */
#define EPOLL_CTL_DEL 3	/* Remove a file descriptor from the interface.  */

/* epoll flags was not supported. */
#define EPOLL_CLOEXEC 1

typedef union epoll_data_t {
  void *ptr;
  int fd;
  uint32_t u32;
  uint64_t u64;
  SOCKET sock; /* Windows specific */
  HANDLE hnd;  /* Windows specific */
} epoll_data_t;

typedef struct epoll_event {
  int32_t      _nouse; // internal use only
  uint32_t     events;
  epoll_data_t data;
} epoll_event;

typedef void*(*epoll_realloc_t) (void *, size_t);

#ifdef __cplusplus
extern "C" {
#endif

UEPOLL_EXPORT void epoll_allocator(epoll_realloc_t realloc_func_t);

UEPOLL_EXPORT HANDLE epoll_create(int size);

UEPOLL_EXPORT HANDLE epoll_create1(int flags);

UEPOLL_EXPORT int epoll_close(HANDLE efd);

UEPOLL_EXPORT int epoll_ctl(HANDLE efd, int op, SOCKET fd, struct epoll_event *event);

UEPOLL_EXPORT int epoll_wait(HANDLE efd, struct epoll_event *events, int maxevents, int timeout);

#ifdef __cplusplus
}
#endif

#endif