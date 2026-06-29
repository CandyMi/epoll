/*
**  LICENSE: MIT
**  Author: CandyMi[https://github.com/candymi]
**
**  epoll.h — Cross-platform dispatch header.
**
**  Single include point for the epoll-compatible API.
**  Selects the right backend at compile time:
**
**    • __linux__ / __ANDROID__  →  native <sys/epoll.h>
**                                  (or uepoll.h with -DNO_NATIVE_EPOLL=1)
**    • WIN32                    →  wepoll.h (IOCP)
**    • otherwise                →  uepoll.h (generic types)
**
**  The actual implementation (epoll.c / kepoll.c / pepoll.c / uepoll.c /
**  wepoll.c) is chosen by the build system.
*/
#ifndef EPOLL_H
#define EPOLL_H

#if defined(__linux__) || defined(__ANDROID__)
  #ifndef NO_NATIVE_EPOLL
    #define NO_NATIVE_EPOLL 0
  #endif
  #if NO_NATIVE_EPOLL == 0
    /* ── Linux native: kernel epoll ────────────────────────────── */
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
  #else
    /* ── Force uepoll on Linux via -DNO_NATIVE_EPOLL=1 ──────────── */
    #include "uepoll.h"
  #endif
#elif defined(WIN32)
  #include "wepoll.h"
#else
  #include "uepoll.h"
#endif

#endif
