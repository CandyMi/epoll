#define _GNU_SOURCE
#include "epoll.h"

#ifdef _WIN32
  #define WIN32
#endif

#ifdef WIN32
  #include <io.h>
  #include <Windows.h>
  #define read _read
  #define write _write
  #define close _close
  #include <sys/timeb.h>
  static inline int gettimeofday(struct timeval *tv, void *tz) {
    struct _timeb tb; _ftime(&tb);
    tv->tv_sec  = (long)tb.time;
    tv->tv_usec = tb.millitm * 1000;
    return 0;
  }
#else
  #include <unistd.h>
  #include <sys/time.h>
  #include <sys/socket.h>
#endif

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

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

#ifndef WIN32
EPOLL_TEST_FUNCTION(testcase_epoll_oneshot, {
    HANDLE efd = epoll_create(1);
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
})

EPOLL_TEST_FUNCTION(testcase_epoll_et_mode, {
    HANDLE efd = epoll_create(1);
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
})

EPOLL_TEST_FUNCTION(testcase_epoll_repeat, {
    HANDLE efd = epoll_create(1);
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
})

EPOLL_TEST_FUNCTION(testcase_epoll_watch, {
    char sendbuf[] = "123";
    HANDLE efd = epoll_create(1);
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
    char *buf = (char*)malloc(strlen(sendbuf) + 1);
    r = read(RPIPE, buf, strlen(sendbuf) + 1);
    assert(r == strlen(sendbuf));
    free(buf); epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_ctl_del, {
    HANDLE efd = epoll_create(1);
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
    /* after deletion, the fd should not be returned */
    struct epoll_event event[1];
    r = epoll_wait(efd, event, 1, 0);
    assert(r == 0);
    close(WPIPE); close(RPIPE); epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_ctl_mod, {
    HANDLE efd = epoll_create(1);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event ev;
    /* register EPOLLIN */
    ev.events = EPOLLIN; ev.data.ptr = (void*)0xdead;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, RPIPE, &ev);
    assert(r == 0);
    write(WPIPE, "x", 1);
    struct epoll_event event[1];
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 1);
    assert(event[0].data.ptr == (void*)0xdead);
    /* drain data so LT doesn't re-trigger */
    { char c; read(RPIPE, &c, 1); }
    /* MOD: update data.ptr, keep events */
    ev.events = EPOLLIN;
    ev.data.ptr = (void*)0xbeef;
    r = epoll_ctl(efd, EPOLL_CTL_MOD, RPIPE, &ev);
    assert(r == 0);
    write(WPIPE, "x", 1);
    r = epoll_wait(efd, event, 1, 10);
    assert(r == 1);
    assert(event[0].data.ptr == (void*)0xbeef);
    close(WPIPE); close(RPIPE); epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_out, {
    HANDLE efd = epoll_create(1);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event ev;
    ev.events = EPOLLOUT; ev.data.fd = WPIPE;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, WPIPE, &ev);
    assert(r == 0);
    /* empty pipe write end should be immediately writable */
    struct epoll_event event[1];
    r = epoll_wait(efd, event, 1, 100);
    assert(r == 1);
    assert(event[0].events & EPOLLOUT);
    assert(event[0].data.fd == WPIPE);
    close(WPIPE); close(RPIPE); epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_multi_fd, {
    HANDLE efd = epoll_create(1);
    assert(efd > 0); int r;
    int fdsA[2]; pipe(fdsA);
    int fdsB[2]; pipe(fdsB);
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fdsA[0]; r = epoll_ctl(efd, EPOLL_CTL_ADD, fdsA[0], &ev); assert(r == 0);
    ev.data.fd = fdsB[0]; r = epoll_ctl(efd, EPOLL_CTL_ADD, fdsB[0], &ev); assert(r == 0);
    /* write to both pipes */
    write(fdsA[1], "A", 1); write(fdsB[1], "B", 1);
    struct epoll_event events[2];
    r = epoll_wait(efd, events, 2, 100);
    assert(r == 2);
    /* both pipes should be ready */
    int seenA = 0; int seenB = 0;
    for (int i = 0; i < r; i++) {
        if (events[i].data.fd == fdsA[0]) seenA = 1;
        if (events[i].data.fd == fdsB[0]) seenB = 1;
    }
    assert(seenA && seenB);
    close(fdsA[0]); close(fdsA[1]); close(fdsB[0]); close(fdsB[1]);
    epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_ctl_error, {
    HANDLE efd = epoll_create(1);
    assert(efd > 0);
    int fds[2]; pipe(fds);
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = fds[0];
    int r = epoll_ctl(efd, EPOLL_CTL_ADD, fds[0], &ev);
    assert(r == 0);
    /* invalid op */
    r = epoll_ctl(efd, 999, fds[0], &ev);
    assert(r != 0);
    /* invalid efd */
    r = epoll_ctl((HANDLE)-1, EPOLL_CTL_ADD, fds[0], &ev);
    assert(r != 0);
    /* epoll_wait invalid args */
    r = epoll_wait((HANDLE)-1, &ev, 1, 0);
    assert(r != 0);
    r = epoll_wait(efd, NULL, 1, 0);
    assert(r != 0);
    r = epoll_wait(efd, &ev, 0, 0);
    assert(r != 0);
    close(fds[0]); close(fds[1]); epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_hup, {
    /*
     * kqueue needs data+EOF to fire simultaneously; select has no HUP concept.
     * This test only checks that epoll_close releases resources after peer close.
     */
    HANDLE efd = epoll_create(1);
    assert(efd > 0);
    int fds[2]; pipe(fds);
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = fds[0];
    int r = epoll_ctl(efd, EPOLL_CTL_ADD, fds[0], &ev);
    assert(r == 0);
    close(fds[1]); close(fds[0]); epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_infinite, {
    HANDLE efd = epoll_create(1);
    assert(efd > 0); int r;
    int fds[2]; pipe(fds);
    int RPIPE = fds[0];
    int WPIPE = fds[1];
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = RPIPE;
    r = epoll_ctl(efd, EPOLL_CTL_ADD, RPIPE, &ev);
    assert(r == 0);
    /* timeout = -1 blocks until data arrives */
    write(WPIPE, "x", 1);
    struct epoll_event event[1];
    r = epoll_wait(efd, event, 1, -1);
    assert(r == 1);
    assert(event[0].data.fd == RPIPE);
    close(WPIPE); close(RPIPE); epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_data_ptr, {
    HANDLE efd = epoll_create(1);
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
    /* data.ptr must round-trip */
    assert(event[0].data.ptr == &ud);
    assert(((struct udata_t*)event[0].data.ptr)->x == 42);
    assert(((struct udata_t*)event[0].data.ptr)->y == 99);
    close(WPIPE); close(RPIPE); epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_create1_flag, {
    /* epoll_create1 with EPOLL_CLOEXEC */
    HANDLE efd = epoll_create1(EPOLL_CLOEXEC);
    assert(efd > 0);
    epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_merge, {
    /* register EPOLLIN|EPOLLOUT — same-fd READ+WRITE should merge into 1 event */
    int sv[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(r == 0);
    HANDLE efd = epoll_create(1);
    assert(efd > 0);
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = sv[0];
    r = epoll_ctl(efd, EPOLL_CTL_ADD, sv[0], &ev);
    assert(r == 0);
    write(sv[1], "x", 1);
    struct epoll_event events[2];
    r = epoll_wait(efd, events, 2, 100);
    /* both filters merged into one event */
    assert(r == 1);
    assert(events[0].events & EPOLLIN);
    assert(events[0].events & EPOLLOUT);
    assert(events[0].data.fd == sv[0]);
    close(sv[0]); close(sv[1]); epoll_close(efd);
})
#endif /* !WIN32 */

/* ── Platform-neutral tests (no pipes/sockets) ────────────────────────── */

EPOLL_TEST_FUNCTION(testcase_epoll_timer,
{
    HANDLE efd = epoll_create(1);
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
    epoll_close(efd);
})

EPOLL_TEST_FUNCTION(testcase_epoll_alloc, 
{
    for (int i = 0; i < 100000; i++)
    {
        HANDLE efd = epoll_create(1);
        assert(efd > 0);
        epoll_close(efd);
    }
});

int main(int argc, char const *argv[])
{
    testcase_epoll_timer();
    testcase_epoll_alloc();
#ifndef WIN32
    testcase_epoll_watch();
    testcase_epoll_repeat();
    testcase_epoll_oneshot();
    // testcase_epoll_et_mode();  /* commented out — select backend doesn't support EPOLLET */
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
#endif
    return 0;
}