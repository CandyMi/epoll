#define GNU_SOURCE
// #include "uepoll.h"
#include "epoll.h"

#ifdef WIN32
    #include <io.h>
    #include <Windows.h>
    #define read _read
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
    #include <unistd.h>
#endif

#include <time.h>
#include <stdint.h>
#ifndef WIN32
    #include <sys/time.h>
#endif

#ifndef STDIN_FILENO
    #define STDIN_FILENO 0 // wepoll 不兼容, 因为只支持socket
#endif

// static inline
// void test_time1(size_t count)
// {
//     struct timespec now; uint64_t t;
//     for (size_t i = 0; i < count; i++)
//     {
//         clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
//         t = now.tv_sec * 1000 + now.tv_nsec / 1000000;
//     }
// }

// static inline
// void test_time2(size_t count)
// {
//     struct timeval now; uint64_t t;
//     for (size_t i = 0; i < count; i++)
//     {
//         gettimeofday(&now, NULL);
//         t = now.tv_sec * 1000 + now.tv_usec / 1000;
//     }
// }

int main(int argc, char const *argv[])
{
    // benchmark for gettime choice 
    // size_t t = strtol(argv[1], NULL, 10);
    // size_t l = strtol(argv[2], NULL, 10);
    // // printf("%zu, %zu\n", t, l);
    // t == 0 ? test_time1(l) : test_time2(l);

    int r;

    /* code */
    HANDLE efd = epoll_create(1024);
    //printf("efd = %zd\n", efd);

    struct epoll_event ev;
    // ev.events = EPOLLHUP; ev.data.fd = STDIN_FILENO;
    // ev.events = EPOLLIN; ev.data.fd = STDIN_FILENO;
    ev.events = EPOLLIN | EPOLLOUT; ev.data.fd = STDIN_FILENO;
    // ev.events = EPOLLIN | EPOLLONESHOT; ev.data.fd = STDIN_FILENO;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, STDIN_FILENO, &ev);
    if (r) perror("ctrl add stdin failed: ");
#ifdef WIN32
    Sleep(1000);
#else
    sleep(1);
#endif
    // struct epoll_event events[1024];
    // r = epoll_wait(efd, events, 1024, -1);
    // printf("r = %zd\n", r);

    while (1)
    {
        struct epoll_event events[1024]; char buf[16];
        r = epoll_wait(efd, events, 1024, -1);
        printf("r = %d", r);
        if (r > 0) {
            printf(", len = %d\n", (int)read(STDIN_FILENO, buf, 16));
        } else {
            printf("\n");
        }
    }

    return 0;
}
