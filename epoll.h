#ifndef _EPOLL_H_
#define _EPOLL_H_

#if defined(__linux__)
  #include <sys/epoll.h>
  #include <unistd.h>
  typedef int HANDLE;
  typedef int SOCKET;
  typedef void*(*epoll_realloc_t) (void *, size_t);
  static inline void epoll_allocator(epoll_realloc_t realloc_func_t) { (void)realloc_func_t; /* 只做兼容 */ }
  static inline int epoll_close(int efd) { return close(efd); /* 兼容ABI */ }
#elif defined(WIN32)
  #include "wepoll.h"
#else
  #include "uepoll.h"
#endif

#endif