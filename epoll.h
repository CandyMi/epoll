#ifndef EPOLL_H
#define EPOLL_H

#if defined(__linux__) || defined(__ANDROID__)
  #define NO_NATIVE_EPOLL 0
#ifndef EPOLL_EXPORT
  #define EPOLL_EXPORT __attribute__((visibility("default")))
#endif
  #include <sys/epoll.h>
  #include <unistd.h>
  typedef int HANDLE;
  typedef int SOCKET;
  typedef void*(*epoll_realloc_t) (void *, size_t);
#ifdef __cplusplus
extern "C" {
#endif
  EPOLL_EXPORT void epoll_allocator(epoll_realloc_t alloc);
  EPOLL_EXPORT int epoll_close(HANDLE efd);
#ifdef __cplusplus
}
#endif
#elif defined(WIN32)
  #include "wepoll.h"
#else
  #include "uepoll.h"
#endif

#endif