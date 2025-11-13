# wepoll


# Demo

```c
#include "uepoll.h"
#include "sys/event.h"
#include "stdio.h"
#include "unistd.h"

int main(int argc, char const *argv[])
{
  HANDLE efd = epoll_create(0);
  printf("efd = %ld\n", efd);
  struct epoll_event revent = { 
    .data.fd = 1024,
    .events = EPOLLIN | EPOLLONESHOT,
  };
  epoll_ctl(efd, EPOLL_CTL_ADD, STDIN_FILENO, &revent);
  // struct epoll_event wevent = { 
  //   .data.fd = 1024,
  //   .events = EPOLLOUT,
  // };
  // epoll_ctl(efd, EPOLL_CTL_ADD, STDOUT_FILENO, &wevent);
  struct epoll_event events[10];
  while (1)
  {
    int r = epoll_wait(efd, events, 10, -1);
    if (r > 0)
    {
      char buf[1024]; memset(buf, 0, 1024);
      int len = read(STDOUT_FILENO, buf, 1024);
      printf("nevents = %d, event{.fd = %d, .events = %d}, data = ['%s']}\n", r, events[0].data.fd, events[0].events, buf);
      // event.data.fd++; event.events = EPOLLIN | EPOLLONESHOT;
      // epoll_ctl(efd, EPOLL_CTL_ADD, STDIN_FILENO, &event);
    }
    printf("ret = %d, out\n", r);
  }

  return 0;
}
```