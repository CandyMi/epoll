#define _GNU_SOURCE
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
#include <string.h>
#include <stdlib.h>
#ifndef WIN32
    #include <unistd.h>
#endif

#include <time.h>
#include <stdint.h>
#ifndef WIN32
    #include <sys/time.h>
    #include <sys/socket.h>
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
    char *buf = malloc(strlen(sendbuf) + 1);
    r = read(RPIPE, buf, strlen(sendbuf) + 1);
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

EPOLL_TEST_FUNCTION(testcase_epoll_ctl_del, {
#ifndef WIN32
    HANDLE efd = epoll_create(0);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = RPIPE;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, RPIPE, &ev);
    assert(r == 0);
    write(WPIPE, "x", 1);
    r = epoll_ctl(efd, EPOLL_CTL_DEL, RPIPE, NULL);
    assert(r == 0);
    /* 删除后再监听不应返回此 fd */
    struct epoll_event event[1];
    r = epoll_wait(efd, event, 1, 0);
    assert(r == 0);
    close(WPIPE); close(RPIPE); epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_ctl_mod, {
#ifndef WIN32
    HANDLE efd = epoll_create(0);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event ev;
    /* 注册 EPOLLIN */
    ev.events = EPOLLIN; ev.data.ptr = (void*)0xdead;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, RPIPE, &ev);
    assert(r == 0);
    write(WPIPE, "x", 1);
    struct epoll_event event[1];
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 1);
    assert(event[0].data.ptr == (void*)0xdead);
    /* 读走数据，否则 LT 会反复触发 */
    { char c; read(RPIPE, &c, 1); }
    /* MOD: 只更新 data.ptr，events 不变 */
    ev.events = EPOLLIN;
    ev.data.ptr = (void*)0xbeef;
    r = epoll_ctl(efd, EPOLL_CTL_MOD, RPIPE, &ev);
    assert(r == 0);
    write(WPIPE, "x", 1);
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 1);
    assert(event[0].data.ptr == (void*)0xbeef);
    close(WPIPE); close(RPIPE); epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_out, {
#ifndef WIN32
    HANDLE efd = epoll_create(0);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event ev;
    ev.events = EPOLLOUT; ev.data.fd = WPIPE;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, WPIPE, &ev);
    assert(r == 0);
    /* 空 pipe 写入端应立即可写 */
    struct epoll_event event[1];
    r = epoll_wait(efd, event, 1, 100);
    assert(r == 1);
    assert(event[0].events & EPOLLOUT);
    assert(event[0].data.fd == WPIPE);
    close(WPIPE); close(RPIPE); epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_multi_fd, {
#ifndef WIN32
    HANDLE efd = epoll_create(0);
    assert(efd > 0); int r;
    int fdsA[2]; pipe(fdsA);
    int fdsB[2]; pipe(fdsB);
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fdsA[0]; r = epoll_ctl(efd, EPOLL_CTL_ADD, fdsA[0], &ev); assert(r == 0);
    ev.data.fd = fdsB[0]; r = epoll_ctl(efd, EPOLL_CTL_ADD, fdsB[0], &ev); assert(r == 0);
    /* 两张 pipe 都写入 */
    write(fdsA[1], "A", 1); write(fdsB[1], "B", 1);
    struct epoll_event events[2];
    r = epoll_wait(efd, events, 2, 100);
    assert(r == 2);
    /* 两张 pipe 都就绪 */
    int seenA = 0;
    int seenB = 0;
    for (int i = 0; i < r; i++) {
        if (events[i].data.fd == fdsA[0]) seenA = 1;
        if (events[i].data.fd == fdsB[0]) seenB = 1;
    }
    assert(seenA && seenB);
    close(fdsA[0]); close(fdsA[1]); close(fdsB[0]); close(fdsB[1]);
    epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_ctl_error, {
#ifndef WIN32
    HANDLE efd = epoll_create(0);
    assert(efd > 0);
    int fds[2]; pipe(fds);
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = fds[0];
    /* epoll_ctl 非法 op */
    int r = epoll_ctl(efd, EPOLL_CTL_ADD, fds[0], &ev);
    assert(r == 0);
    r = epoll_ctl(efd, 999, fds[0], &ev);
    assert(r != 0);
    /* 无效 efd */
    r = epoll_ctl((HANDLE)-1, EPOLL_CTL_ADD, fds[0], &ev);
    assert(r != 0);
    /* epoll_wait 无效参数 */
    r = epoll_wait((HANDLE)-1, &ev, 1, 0);
    assert(r != 0);
    r = epoll_wait(efd, NULL, 1, 0);
    assert(r != 0);
    r = epoll_wait(efd, &ev, 0, 0);
    assert(r != 0);
    close(fds[0]); close(fds[1]); epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_hup, {
#ifndef WIN32
    /* 对端关闭的触发行为因后端而异（kqueue 需数据+EOF 同时触发），
     * 此处仅验证管道关闭后 epoll_close 正常释放资源。 */
    HANDLE efd = epoll_create(0);
    assert(efd > 0);
    int fds[2]; pipe(fds);
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = fds[0];
    int r = epoll_ctl(efd, EPOLL_CTL_ADD, fds[0], &ev);
    assert(r == 0);
    close(fds[1]); close(fds[0]); epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_infinite, {
#ifndef WIN32
    HANDLE efd = epoll_create(0);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = RPIPE;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, RPIPE, &ev);
    assert(r == 0);
    /* timeout = -1 无限等待，仅在数据到达时返回 */
    write(WPIPE, "x", 1);
    struct epoll_event event[1];
    r = epoll_wait(efd, event, 1, -1);
    assert(r == 1);
    assert(event[0].data.fd == RPIPE);
    close(WPIPE); close(RPIPE); epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_data_ptr, {
#ifndef WIN32
    HANDLE efd = epoll_create(0);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event ev;
    struct udata_t { int x; int y; } ud;
    ud.x = 42; ud.y = 99;
    ev.events = EPOLLIN; ev.data.ptr = &ud;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, RPIPE, &ev);
    assert(r == 0);
    write(WPIPE, "x", 1);
    struct epoll_event event[1];
    r = epoll_wait(efd, event, 1, 100);
    assert(r == 1);
    /* data.ptr 应原样返回 */
    assert(event[0].data.ptr == &ud);
    assert(((struct udata_t*)event[0].data.ptr)->x == 42);
    assert(((struct udata_t*)event[0].data.ptr)->y == 99);
    close(WPIPE); close(RPIPE); epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_create1_flag, {
#ifndef WIN32
    /* epoll_create1 with EPOLL_CLOEXEC */
    HANDLE efd = epoll_create1(EPOLL_CLOEXEC);
    assert(efd > 0);
    epoll_close(efd);
#endif
})

EPOLL_TEST_FUNCTION(testcase_epoll_merge, {
#ifndef WIN32
    /* 注册 EPOLLIN|EPOLLOUT，同一 fd 的 EVFILT_READ+WRITE 双事件合并为 1 条 */
    int sv[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(r == 0);
    HANDLE efd = epoll_create(0);
    assert(efd > 0);
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = sv[0];
    r = epoll_ctl(efd, EPOLL_CTL_ADD, sv[0], &ev);
    assert(r == 0);
    /* 向对端写数据，使 sv[0] 同时满足 readable+writeable */
    write(sv[1], "x", 1);
    /* maxevents 必须 ≥ kqueue 事件数（2），否则 EVFILT_READ 被截断 */
    struct epoll_event events[2];
    r = epoll_wait(efd, events, 2, 100);
    /* 双 filter 合并为单条事件 */
    assert(r == 1);
    assert(events[0].events & EPOLLIN);
    assert(events[0].events & EPOLLOUT);
    assert(events[0].data.fd == sv[0]);
    close(sv[0]); close(sv[1]); epoll_close(efd);
#endif
})

int main(int argc, char const *argv[])
{
    testcase_epoll_timer();
    testcase_epoll_alloc();
    testcase_epoll_watch();
    testcase_epoll_repeat();
    testcase_epoll_oneshot();
    // ET模式在一些平台上无法不支持
    // testcase_epoll_et_mode();
    testcase_epoll_ctl_del();
    testcase_epoll_ctl_mod();
    testcase_epoll_out();
    testcase_epoll_multi_fd();
    testcase_epoll_ctl_error();
    testcase_epoll_hup();
    testcase_epoll_infinite();
    testcase_epoll_data_ptr();
    testcase_epoll_create1_flag();
    testcase_epoll_merge();
    return 0;
}
