/*
**  LICENSE: MIT
**  Author: CandyMi[https://github.com/candymi]
**
**  epoll.c — Linux native epoll shim.
**
**  Thin wrapper around the kernel's epoll_create1 / epoll_ctl / epoll_wait.
**  Only epoll_allocator (no-op) and epoll_close are overridden; everything
**  else delegates directly to <sys/epoll.h>.
**
**  Selected when __linux__ / __ANDROID__ and NO_NATIVE_EPOLL == 0.
*/

#include "epoll.h"

#include <unistd.h>

void epoll_allocator(epoll_realloc_t alloc)
{
  (void)alloc; /* no-op on Linux — kernel epoll does no user-space allocation */
}

int epoll_close(HANDLE efd)
{
  return close(efd);
}