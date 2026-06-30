/*
**  LICENSE: MIT
**  Author: CandyMi[https://github.com/candymi]
**
**  epoll_internal.h — Compile-time shared definitions for all POSIX backends.
**
**  Included by kepoll.c, pepoll.c, and uepoll.c.  Eliminates the
**  three-way copy of spinlock, memory helpers, and utility functions
**  that were previously repeated in each backend.
**
**  NOT included by epoll.h / uepoll.h / wepoll.h — internal only.
*/
#ifndef EPOLL_INTERNAL_H
#define EPOLL_INTERNAL_H

#include "uepoll.h"

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/time.h>

/* ==================================================================
 *  1.  Common constants
 *
 *  Every backend defines these with the same value.  Using a shared
 *  definition eliminates three copies of essentially identical code.
 *
 *  Note: EPOLL_INVALID is deliberately not a macro with parens so
 *  that return statements like "return EPOLL_INVALID;" read naturally
 *  and don't expand to "return (-1);" with a redundant paren.
 * ================================================================== */

#ifndef EPOLL_INVALID
  #define EPOLL_INVALID (-1)
#endif

/* EPOLLEMPTY and EPOLL_SUCCESS omitted — they're only used by
 * 2/3 backends; each backend defines its own if needed. */

/* Per-call stack buffer size for epoll_wait working buffers.
 * Both kepoll.c (kevent) and pepoll.c (pollfd) use this to
 * avoid heap allocation for small event sets (≤ 256 entries).
 * sizeof(struct kevent) ≈ 32 bytes → ~8 KB stack cost. */
#define EPOLL_STACK_NFDS  256

/* ==================================================================
 *  2.  Memory helpers
 *
 *  Wrappers around the user-supplied allocator (epoll_allocator).
 *  epoll_malloc(sz)   → epoll_realloc(NULL, sz)
 *  epoll_free(ptr)    → epoll_realloc(ptr, 0)
 *
 *  These match realloc(3) semantics exactly, so the user's custom
 *  allocator need only implement realloc.
 * ================================================================== */

#ifndef epoll_malloc
  #define epoll_malloc(sz)  epoll_realloc(NULL, sz)
#endif

#ifndef epoll_free
  #define epoll_free(ptr)   (void)epoll_realloc(ptr, 0)
#endif

/* ==================================================================
 *  3.  FD helper macros
 *
 *  Convenience wrappers around fcntl(2) that backends commonly use
 *  when creating pipes or epoll instances.
 * ================================================================== */

#ifndef epoll_nonexec
  #define epoll_nonexec(fd)   fcntl(fd, F_SETFD, FD_CLOEXEC | fcntl(fd, F_GETFD))
#endif

#ifndef epoll_nonblock
  #define epoll_nonblock(fd)  fcntl(fd, F_SETFL, O_NONBLOCK | fcntl(fd, F_GETFL))
#endif

/* ==================================================================
 *  4.  Round up to the next power of two
 *
 *  Used by the shared-working-buffer path (EPOLL_USE_SHARED_WORKBUF=1)
 *  in both kepoll.c and pepoll.c to size the realloc'd buffer.
 *  Returns 1 for n == 0, and works for any size_t on 32- and 64-bit.
 * ================================================================== */

#ifndef HAVE_ROUND_UP_POW2
  #define HAVE_ROUND_UP_POW2
  static inline size_t round_up_pow2(size_t n) {
      if (n == 0) return 1;
      n--;
      n |= n >> 1; n |= n >> 2; n |= n >> 4;
      n |= n >> 8; n |= n >> 16;
  #if SIZE_MAX > UINT32_MAX
      n |= n >> 32;
  #endif
      return n + 1;
  }
#endif

/* ==================================================================
 *  5.  Spinlock (3-level degrading)
 *
 *  Every file shares the identical spinlock strategy.  Prefer the most
 *  efficient facility available on the compiler/platform:
 *
 *    Level 0 — EPOLL_NO_THREADS:  no-op (single-threaded assumption)
 *    Level 1 — C11 <stdatomic.h>: atomic_flag test-and-set
 *    Level 2 — GCC __sync_*:      __sync_lock_test_and_set
 *    Fallback — #error
 *
 *  No pthread or OS-level mutex is used.  Critical sections are short
 *  (registering a kevent or copying an fd_set), so spinlocks are
 *  appropriate.
 * ================================================================== */

// #define EPOLL_NO_THREADS 1
#if defined(EPOLL_NO_THREADS)
  /* Single-threaded: all locking is a no-op. */
  typedef int epoll_lock_t;
  #define epoll_spinlock_init(lock)       ((void)(lock))
  #define epoll_spinlock_lock(lock)       ((void)(lock))
  #define epoll_spinlock_unlock(lock)     ((void)(lock))

#elif defined(EPOLL_USE_ATOMIC) || \
  (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__))
  /* C11 <stdatomic.h> — the preferred path. */
  #include <stdatomic.h>
  typedef atomic_flag epoll_lock_t;
  #define epoll_spinlock_init(lock)       atomic_flag_clear_explicit((lock), memory_order_release)
  #define epoll_spinlock_lock(lock)       do {} while (atomic_flag_test_and_set_explicit((lock), memory_order_acquire))
  #define epoll_spinlock_unlock(lock)     atomic_flag_clear_explicit((lock), memory_order_release)

#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1) \
   || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_2) \
   || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4) \
   || defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8)
  /* GCC legacy __sync_* builtins (pre-C11 compilers). */
  typedef int epoll_lock_t;
  #define epoll_spinlock_init(lock)       __sync_lock_test_and_set((lock), 0)
  #define epoll_spinlock_lock(lock)       do { __sync_synchronize(); } while (__sync_lock_test_and_set((lock), 1) == 1)
  #define epoll_spinlock_unlock(lock)     __sync_lock_release((lock))

#else
  #error "epoll internal: no atomic operations available.  Define EPOLL_NO_THREADS if single-threaded."
#endif

/* ==================================================================
 *  5.5  Reference count (atomic, 3-level degrading)
 *
 *  Shared by kepoll.c, pepoll.c, uepoll.c for safe concurrent
 *  epoll_close / epoll_wait.  Prevents use-after-free by deferring
 *  struct epoll_t deallocation until the last reference is dropped.
 *
 *  Detection mirrors the spinlock (section 5 above): C11 <stdatomic.h>
 *  is preferred; GCC __sync_* builtins are the fallback.
 *
 *  epoll_refcnt_inc(r)  — return NEW value (post-increment)
 *  epoll_refcnt_dec(r)  — return NEW value (post-decrement)
 *  Rationale: checking "== 0" after dec reads naturally as "is zero".
 * ================================================================== */

#if defined(EPOLL_USE_ATOMIC) || \
  (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__))
  /* C11 <stdatomic.h> — included above by the spinlock section. */
  typedef atomic_int epoll_refcnt_t;
  #define epoll_refcnt_init(r, v)  atomic_init(r, v)
  #define epoll_refcnt_inc(r)      (atomic_fetch_add(r, 1) + 1)
  #define epoll_refcnt_dec(r)      (atomic_fetch_sub(r, 1) - 1)

#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4) || \
      defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8)
  /* GCC legacy __sync_* builtins (pre-C11 compilers). */
  typedef int epoll_refcnt_t;
  #define epoll_refcnt_init(r, v)  ((void)(*(r) = (v)))
  #define epoll_refcnt_inc(r)      __sync_add_and_fetch(r, 1)
  #define epoll_refcnt_dec(r)      __sync_sub_and_fetch(r, 1)

#elif defined(EPOLL_NO_THREADS)
  /* Single-threaded: plain volatile int. */
  typedef volatile int epoll_refcnt_t;
  #define epoll_refcnt_init(r, v)  ((void)(*(r) = (v)))
  #define epoll_refcnt_inc(r)      (++*(r))
  #define epoll_refcnt_dec(r)      (--*(r))
#else
  #error "epoll internal: no atomic operations available for refcount."
#endif

/* ==================================================================
 *  6.  Monotonic clock — current time in milliseconds
 *
 *  Backends need a monotonic (or at least non-decreasing) time source
 *  for EINTR-retry timeout decay.  Resolution: milliseconds.
 *
 *  Preference order:
 *    1. CLOCK_MONOTONIC_COARSE  (fast, Linux)
 *    2. CLOCK_REALTIME_COARSE   (fast, Linux — uepoll.c fallback)
 *    3. CLOCK_MONOTONIC         (POSIX, every modern system)
 *    4. gettimeofday            (portable, but may jump on wall-clock
 *                                changes — acceptable for timeout decay)
 * ================================================================== */

static inline int64_t epoll_now_ms(void)
{
#if defined(CLOCK_MONOTONIC_COARSE)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#elif defined(CLOCK_REALTIME_COARSE)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

/* ==================================================================
 *  7.  EINTR / pipe-wake timeout decay helpers
 *
 *  Shared by kepoll.c, pepoll.c, and uepoll.c.  Eliminates the
 *  3-way copy of the same elapsed-time calculation pattern.
 *
 *  Usage:
 *    int64_t t0 = epoll_timeout_start();
 *    int r = poll(...);
 *    if (r < 0 && errno == EINTR)
 *        epoll_timeout_decay(&timeout, t0);
 * ================================================================== */

/* Record the start timestamp before a blocking system call.
 * Call right before kevent/poll/select, not before the lock acquire. */
static inline int64_t epoll_timeout_start(void)
{
    return epoll_now_ms();
}

/* Decay `*timeout_ms` by the wall-clock time elapsed since `start_ms`.
 * Clamps to 0.  Safe to call when *timeout_ms <= 0 (no-op). */
static inline void epoll_timeout_decay(int *timeout_ms, int64_t start_ms)
{
    if (*timeout_ms > 0) {
        int64_t elapsed = epoll_now_ms() - start_ms;
        if (elapsed > 0) {
            *timeout_ms -= (int)elapsed;
            if (*timeout_ms < 0) *timeout_ms = 0;
        }
    }
}

#endif /* EPOLL_INTERNAL_H */
