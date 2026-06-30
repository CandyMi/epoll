/*
**  bench.c — epoll cross-platform performance benchmark.
**
**  Measures:
**    1. LT event latency         — ns per epoll_wait round-trip
**    2. ONESHOT re-registration  — ns per ONESHOT fire + MOD re-arm
**    3. Allocation pressure      — ns per epoll_create / epoll_close
*/

#include "epoll.h"
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static double bench_lt(int n)
{
    int fds[2]; pipe(fds);
    HANDLE efd = epoll_create(1);
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0};
    epoll_ctl(efd, EPOLL_CTL_ADD, fds[0], &ev);
    char c = 'x';
    struct epoll_event out;
    for (int i = 0; i < 500; i++) { write(fds[1],&c,1); epoll_wait(efd,&out,1,-1); read(fds[0],&c,1); }
    int64_t t0 = now_us();
    for (int i = 0; i < n; i++) { write(fds[1],&c,1); epoll_wait(efd,&out,1,-1); read(fds[0],&c,1); }
    int64_t t1 = now_us();
    close(fds[0]); close(fds[1]); epoll_close(efd);
    return (double)(t1 - t0) * 1000.0 / n;
}

static double bench_oneshot(int n)
{
    int fds[2]; pipe(fds);
    HANDLE efd = epoll_create(1);
    struct epoll_event ev = {.events = EPOLLIN|EPOLLONESHOT, .data.u64 = 0};
    epoll_ctl(efd, EPOLL_CTL_ADD, fds[0], &ev);
    char c = 'x';
    struct epoll_event out;
    /* warm-up: fire ONESHOT, then MOD to re-arm before measured loop */
    write(fds[1],&c,1); epoll_wait(efd,&out,1,-1); read(fds[0],&c,1);
    ev.events = EPOLLIN|EPOLLONESHOT;
    epoll_ctl(efd, EPOLL_CTL_MOD, fds[0], &ev);
    int64_t t0 = now_us();
    for (int i = 0; i < n; i++) {
        write(fds[1],&c,1); epoll_wait(efd,&out,1,-1); read(fds[0],&c,1);
        ev.events = EPOLLIN|EPOLLONESHOT;
        epoll_ctl(efd, EPOLL_CTL_MOD, fds[0], &ev);
    }
    int64_t t1 = now_us();
    close(fds[0]); close(fds[1]); epoll_close(efd);
    return (double)(t1 - t0) * 1000.0 / n;
}

static double bench_alloc(int n)
{
    int64_t t0 = now_us();
    for (int i = 0; i < n; i++) { HANDLE h = epoll_create(1); if (h > 0) epoll_close(h); }
    int64_t t1 = now_us();
    return (double)(t1 - t0) * 1000.0 / n;
}

int main(void)
{
    printf("=== epoll Benchmark ===\n"); fflush(stdout);
    double lt = bench_lt(5000);
    printf("  LT event           : %.0f ns/event  (5000 iters)\n", lt); fflush(stdout);
    double os = bench_oneshot(3000);
    printf("  ONESHOT re-register: %.0f ns/event  (3000 iters)\n", os); fflush(stdout);
    double al = bench_alloc(10000);
    printf("  create/close       : %.0f ns/op     (10000 iters)\n", al); fflush(stdout);
    printf("=== Done ===\n");
    return 0;
}
