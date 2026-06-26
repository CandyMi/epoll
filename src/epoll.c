#include "epoll.h"

#include <unistd.h>

void epoll_allocator(epoll_realloc_t alloc)
{
  (void)alloc; /* 只做兼容 */
}

int epoll_close(HANDLE efd)
{
  return close(efd);
}