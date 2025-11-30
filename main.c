#define GNU_SOURCE
// #include "uepoll.h"
#include "epoll.h"

#ifdef WIN32
    #include <io.h>
    #include <Windows.h>
    #define read _read
#endif

#include <assert.h>
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

#define EPOLL_TEST_FUNCTION(fname, code)    \
static void fname() { code; printf(         \
"%02d. test case : '%s' successed.\n",++i, __FUNCTION__); }

static int i = 0;

static inline
int64_t time_now()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

EPOLL_TEST_FUNCTION(testcase_epoll_oneshot, {
#ifndef WIN32
    HANDLE efd = epoll_create(0);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event event[1];
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = RPIPE;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, RPIPE, &ev);
    assert(r == 0);
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 0);
    write(WPIPE, "1", 1);
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 1);
    assert(event[0].data.fd == RPIPE);
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 0);
    epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_et_mode, {
#ifndef WIN32
    HANDLE efd = epoll_create(0);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event event[1];
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = RPIPE;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, RPIPE, &ev);
    assert(r == 0);
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 0);
    write(WPIPE, "1", 1);
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 1);
    assert(event[0].data.fd == RPIPE);
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 0);
    r = epoll_ctl(efd, EPOLL_CTL_MOD, RPIPE, &ev);
    assert(r == 0);
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 1);
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 0);
    epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_repeat, {
#ifndef WIN32
    HANDLE efd = epoll_create(0);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event event[1];
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = RPIPE;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, RPIPE, &ev);
    assert(r == 0);
    write(WPIPE, "1", 1);
    for (size_t i = 0; i < 100000; i++)
    {
        r = epoll_wait(efd, event, 1, 0);
        assert(event[0].data.fd == RPIPE);
        assert(r == 1);
    }
    char buf [128];
    r = read(RPIPE, buf, 128);
    assert(r >= 1);
    r = epoll_wait(efd, event, 1, 0);
    assert(r == 0);
    epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_watch, {
#ifndef WIN32
    char sendbuf[] = "123";
    HANDLE efd = epoll_create(0);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = RPIPE;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, RPIPE, &ev);
    assert(r == 0);
    struct epoll_event event[1];
    r = write(WPIPE, sendbuf, strlen(sendbuf));
    assert(r == strlen(sendbuf));
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 1);
    assert(event[0].data.fd == RPIPE);
    assert(event[0].events == EPOLLIN);
    char *buf = malloc(strlen(sendbuf));
    r = read(RPIPE, buf, 128);
    assert(r == strlen(sendbuf));
    free(buf); epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_alloc, 
{
    for (int i = 0; i < 100000; i++)
    {
        HANDLE efd = epoll_create(0);
        assert(efd > 0);
        epoll_close(efd);
    }
});

EPOLL_TEST_FUNCTION(testcase_epoll_timer,
{
    HANDLE efd = epoll_create(0);
    assert(efd > 0);
    struct epoll_event event[1];
    int64_t s = time_now();
    for (size_t i = 0; i < 10; i++)
    {
        int r = epoll_wait(efd, event, 1, 100);
        assert(r == 0);
    }
    int64_t e = time_now();
    printf("clock diff = '%d'\n", (int)(e - s));
    // assert(r > 0)
    epoll_close(efd);
})

int main(int argc, char const *argv[])
{
    testcase_epoll_timer();
    testcase_epoll_alloc();
    testcase_epoll_watch();
    testcase_epoll_repeat();
    testcase_epoll_oneshot();
    testcase_epoll_et_mode();
    return 0;
}
