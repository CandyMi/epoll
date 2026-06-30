/*
**  LICENSE: MIT
**  Author: CandyMi[https://github.com/candymi]
**
**  pepoll.c — poll(2)-based epoll emulation for generic POSIX systems.
**
**  Unlike select (uepoll.c), poll(2):
**    • Has no FD_SETSIZE limit — supports up to RLIMIT_NOFILE fds.
**    • Uses milliseconds directly — no timeval conversion.
**    • Reports POLLHUP / POLLPRI / POLLRDHUP natively.
**    • More efficient for sparse fd sets (no bitmap scanning).
**
**  Limitations (inherited from the poll(2) API):
**    • EPOLLET (edge-triggered) is unsupported — poll is always LT.
**    • EPOLLEXCLUSIVE / EPOLLWAKEUP are defined for compat only.
**
**  This backend is selected for POSIX systems where:
**    • poll(2) is available (practically everywhere POSIX).
**    • Neither Linux native epoll nor kqueue is present.
**
**  Thread safety: same 3-level degrading spinlock as other backends.
**  Self-waking pipe mechanism follows the uepoll.c pattern.
*/

#include "uepoll.h"
#include "epoll_internal.h"

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/time.h>

/* ── Backend-specific constants ────────────────────────────────────── */

#define EPOLL_SUCCESS     (0)

/* Initial & max capacity for the dynamic pollfd array.
 * Slot 0 is reserved for the self-waking pipe read end.
 * PP_MAX_CAPACITY controls max REGISTERED fds (epoll_ctl ADD),
 * NOT per-wait event count — that is EPOLL_MAX_EVENTS below. */
#define PP_INITIAL_CAPACITY   64
#define PP_MAX_CAPACITY       (256 * 1024)

/* Per-wait max events cap (power of two).
 * epoll_wait returns at most this many events per call. */
#ifndef EPOLL_MAX_EVENTS
  #define EPOLL_MAX_EVENTS 65536
#endif

/* Initial capacity for the shared working buffer (if EPOLL_USE_SHARED_WORKBUF=1). */
#ifndef EPOLL_INITIAL_CAP
  #define EPOLL_INITIAL_CAP 512
#endif

/* Epoll → poll event mapping.
 *
 * EPOLLPRI  → POLLPRI  (urgent data)
 * EPOLLERR  → POLLERR  (error condition)
 * EPOLLHUP  → POLLHUP  (hang up)
 * EPOLLIN   → POLLIN   (data to read)
 * EPOLLOUT  → POLLOUT  (ready to write)
 *
 * Sub-flags (RDNORM, RDBAND, WRNORM, WRBAND) all map to the same
 * POLLIN / POLLOUT bits — poll(2) does not distinguish priority bands.
 */
#define PP_EPOLL2POLL(ev)                                               \
    ( ((ev) & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND) ? POLLIN    : 0) |  \
      ((ev) & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND) ? POLLOUT   : 0) | \
      ((ev) & EPOLLERR                               ? POLLERR    : 0) | \
      ((ev) & EPOLLHUP                               ? POLLHUP    : 0) | \
      ((ev) & EPOLLPRI                               ? POLLPRI    : 0)   \
    )

/* Poll → epoll reverse mapping.
 * Matches uepoll.c convention: returns the primary flag only
 * (EPOLLIN / EPOLLOUT) without sub-flags RDNORM/WRNORM, so
 * callers expecting exact EPOLLIN = 0x001 comparisons pass.
 * POLLNVAL maps to EPOLLERR (bad fd is an error condition). */
#define PP_REVENTS2EPOLL(rev)                                           \
    ( ((rev) & POLLIN    ? EPOLLIN   : 0) |                             \
      ((rev) & POLLOUT   ? EPOLLOUT  : 0) |                             \
      ((rev) & POLLERR   ? EPOLLERR  : 0) |                             \
      ((rev) & POLLHUP   ? EPOLLHUP  : 0) |                             \
      ((rev) & POLLPRI   ? EPOLLPRI  : 0) |                             \
      ((rev) & POLLNVAL  ? EPOLLERR  : 0)                               \
    )

#define PP_HAS_ONESHOT(ev)  ((ev) & EPOLLONESHOT)

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Validate that a file descriptor can be polled via poll(2).
 * poll(2) accepts sockets, pipes/FIFOs, character devices, and
 * regular files on most systems.  Block devices are rejected early. */
static inline bool ppoll_can_poll(SOCKET fd)
{
    struct stat st;
    if (fstat((int)fd, &st) != 0)
        return false;
    return S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode)
        || S_ISCHR(st.st_mode) || S_ISREG(st.st_mode);
}

#define epoll_nonexec(fd)   fcntl(fd, F_SETFD, FD_CLOEXEC | fcntl(fd, F_GETFD))
#define epoll_nonblock(fd)  fcntl(fd, F_SETFL, O_NONBLOCK | fcntl(fd, F_GETFL))

/* ── Per-entry data ────────────────────────────────────────────────── */

struct poll_entry {
    SOCKET        fd;            /* file descriptor, or -1 if slot is free */
    uint32_t      epoll_events;  /* requested epoll event mask (for ONESHOT tracking) */
    epoll_data_t  data;          /* user data (round-trip for epoll_wait) */
};

/* ── Epoll instance ────────────────────────────────────────────────── */

struct epoll_t {
    bool              edited;       /* true if epoll_ctl modified state since last drain */
    int               pipes[2];     /* self-waking pipe (write to wake, read to drain) */
    epoll_lock_t      lock;
    volatile int      closing;      /* 1 = epoll_close in progress */

    int               capacity;     /* allocated size of arrays */
    int               nfds;         /* number of active entries (including pipe slot 0) */

    struct poll_entry *entries;     /* [capacity] — metadata for each slot */
    struct pollfd     *pollfds;     /* [capacity] — parallel array, passed to poll(2) */

    /*
     * Free-list head — O(1) slot allocation.
     *
     * Free slots are singly-linked via entries[].epoll_events (uint32_t
     * next-index).  End-of-list sentinel is ~0U.  Slot 0 is permanently
     * reserved for the self-waking pipe, so free_head starts at 1.
     *
     * Because epoll_events doubles as event-mask for in-use slots and
     * next-index for free slots, there's no extra memory cost.
     */
    int               free_head;

#if EPOLL_USE_SHARED_WORKBUF
    struct pollfd    *working;      /* shared pollfd working buffer (2^n heap, lazy) */
    size_t            wcap;         /* current working capacity */
#endif
};

/*
 * Per-thread working buffer for poll(2).
 * Each epoll_wait call needs its own local copy; using a shared buffer
 * in struct epoll_t would cause data races on concurrent epoll_wait.
 *
 * Use a stack buffer for small arrays (up to EPOLL_STACK_NFDS) to avoid
 * heap churn; fall back to epoll_malloc for larger sets.
 */

/* ── Allocator ─────────────────────────────────────────────────────── */

static epoll_realloc_t epoll_realloc = realloc;
void epoll_allocator(epoll_realloc_t realloc_func)
{
    if (!realloc_func) return;
    epoll_realloc = realloc_func;
}

/* ── Internal helpers ──────────────────────────────────────────────── */

/* Drain the self-waking pipe (consume all wake-up bytes). */
static inline void ppoll_drain_pipe(struct epoll_t *ep)
{
    if (ep->edited) {
        ep->edited = false;
        char buf[1024];
        while (read(ep->pipes[0], buf, sizeof(buf)) > 0) {}
    }
}

/* Find the slot index for a given fd. Returns -1 if not found.
 * Linear scan — slot 0 is reserved for the pipe fd. */
static inline int ppoll_find_slot(const struct epoll_t *ep, SOCKET fd)
{
    for (int i = 1; i < ep->capacity; i++) {
        if (ep->entries[i].fd == fd)
            return i;
    }
    return -1;
}

/* Grow arrays to `new_cap` elements.
 * Uses epoll_realloc (== realloc semantics) so the allocator may
 * extend in-place — avoiding the malloc+memcpy+free triple of the
 * original design.  On ENOMEM, best-effort rollback is attempted.
 * Returns 0 on success, -1 on ENOMEM.
 *
 * NOTE: caller must ensure new_cap > 0 and that the multiplication
 * in `ep->capacity * 2` didn't overflow (see ppoll_add_locked). */
static inline int ppoll_grow(struct epoll_t *ep, int new_cap)
{
    if (new_cap > PP_MAX_CAPACITY || new_cap < PP_INITIAL_CAPACITY) {
        errno = ENOMEM;
        return -1;
    }

    size_t entry_sz = (size_t)new_cap * sizeof(struct poll_entry);
    size_t pollfd_sz = (size_t)new_cap * sizeof(struct pollfd);
    int old_cap = ep->capacity;

    /* Realloc entries first */
    struct poll_entry *new_entries = (struct poll_entry *)
        epoll_realloc(ep->entries, entry_sz);
    if (!new_entries) {
        errno = ENOMEM;
        return -1;
    }
    ep->entries = new_entries;

    /* Realloc pollfds — on failure, roll back entries to old size */
    struct pollfd *new_pollfds = (struct pollfd *)
        epoll_realloc(ep->pollfds, pollfd_sz);
    if (!new_pollfds) {
        /* Best-effort rollback: shrink entries back to old_cap.
         * If this also fails, entries is oversized but still valid. */
        size_t old_sz = (size_t)old_cap * sizeof(struct poll_entry);
        struct poll_entry *rolled = (struct poll_entry *)
            epoll_realloc(ep->entries, old_sz);
        if (rolled) ep->entries = rolled;
        errno = ENOMEM;
        return -1;
    }
    ep->pollfds = new_pollfds;

    /* Prep new slots to the free list (prepend, reverse-order so
     * allocation order is old_cap, old_cap+1, …) */
    for (int i = new_cap - 1; i >= old_cap; i--) {
        ep->entries[i].fd = -1;
        ep->entries[i].epoll_events = ep->free_head;
        ep->pollfds[i].fd = -1;
        ep->free_head = i;
    }

    ep->capacity = new_cap;
    return 0;
}


/* ── Core operations: add / mod / del ──────────────────────────────── */

/*
 * ppoll_register / ppoll_unregister — assume the caller holds ep->lock.
 * These directly manipulate entries[] and pollfds[].
 */
static inline void ppoll_register_locked(struct epoll_t *ep, int slot,
                                          SOCKET fd, struct epoll_event *event)
{
    ep->entries[slot].fd           = fd;
    ep->entries[slot].epoll_events = event->events;
    ep->entries[slot].data         = event->data;

    ep->pollfds[slot].fd     = fd;
    ep->pollfds[slot].events = PP_EPOLL2POLL(event->events);
    ep->pollfds[slot].revents = 0;

    /* Track nfds: count active entries (slot 0 is always the pipe) */
    if (slot >= ep->nfds)
        ep->nfds = slot + 1;

    ep->edited = true;
    write(ep->pipes[1], "x", 1);
}

static inline void ppoll_unregister_locked(struct epoll_t *ep, int slot)
{
    /* Push the freed slot onto the free list (LIFO, O(1)) */
    ep->entries[slot].fd           = -1;
    ep->entries[slot].epoll_events = ep->free_head;
    ep->entries[slot].data.ptr     = NULL;
    ep->free_head = slot;

    ep->pollfds[slot].fd      = -1;
    ep->pollfds[slot].events  = 0;
    ep->pollfds[slot].revents = 0;

    /* Shrink nfds if the highest slot was freed */
    if (slot + 1 == ep->nfds) {
        while (ep->nfds > 1 && ep->entries[ep->nfds - 1].fd == -1)
            ep->nfds--;
    }

    ep->edited = true;
    write(ep->pipes[1], "x", 1);
}

/*
 * ppoll_add_locked / ppoll_mod_locked / ppoll_del_locked —
 * assume caller (epoll_ctl) already holds ep->lock.
 * They do NOT release the lock on return — epoll_ctl does.
 */
static inline int ppoll_add_locked(struct epoll_t *ep, SOCKET fd, struct epoll_event *event)
{
    if (!ppoll_can_poll(fd)) {
        errno = EPERM;
        return EPOLL_INVALID;
    }
    /* Lock is held by caller */
    if (ep->closing) {
        errno = EBADF;
        return EPOLL_INVALID;
    }
    if (ppoll_find_slot(ep, fd) >= 0) {
        errno = EEXIST;
        return EPOLL_INVALID;
    }

    /* Pop from free list (O(1)) */
    int slot = ep->free_head;
    if (slot < 0 || (uint32_t)slot >= (uint32_t)ep->capacity) {
        /* Free list empty — grow.  Keep the lock held so no
         * concurrent epoll_close can free ep during realloc.
         * Check for overflow before multiplying. */
        int new_cap;
        if (ep->capacity > PP_MAX_CAPACITY / 2) {
            errno = ENOMEM;
            return EPOLL_INVALID;
        }
        new_cap = ep->capacity * 2;
        if (new_cap < PP_INITIAL_CAPACITY)
            new_cap = PP_INITIAL_CAPACITY;
        if (ppoll_grow(ep, new_cap) != 0)
            return EPOLL_INVALID;

        slot = ep->free_head;
        if (slot < 0 || (uint32_t)slot >= (uint32_t)ep->capacity) {
            errno = ENOMEM;
            return EPOLL_INVALID;
        }
    }

    /* Advance the free list before register writes epoll_events */
    ep->free_head = (int)ep->entries[slot].epoll_events;

    ppoll_register_locked(ep, slot, fd, event);
    return EPOLL_SUCCESS;
}

static inline int ppoll_mod_locked(struct epoll_t *ep, SOCKET fd, struct epoll_event *event)
{
    if (ep->closing) {
        errno = EBADF;
        return EPOLL_INVALID;
    }
    int slot = ppoll_find_slot(ep, fd);
    if (slot < 0) {
        errno = ENOENT;
        return EPOLL_INVALID;
    }

    ppoll_register_locked(ep, slot, fd, event);
    return EPOLL_SUCCESS;
}

static inline int ppoll_del_locked(struct epoll_t *ep, SOCKET fd)
{
    if (ep->closing) {
        errno = EBADF;
        return EPOLL_INVALID;
    }
    int slot = ppoll_find_slot(ep, fd);
    if (slot < 0) {
        errno = ENOENT;
        return EPOLL_INVALID;
    }

    ppoll_unregister_locked(ep, slot);
    return EPOLL_SUCCESS;
}

/* Public wrappers — each takes lock, delegates to locked variant,
 * releases lock.  Kept for any direct callers within the file. */
/* ── Public API ────────────────────────────────────────────────────── */

int epoll_ctl(HANDLE efd, int op, SOCKET fd, struct epoll_event *event)
{
    errno = 0;

    if (efd <= 0 || fd < 0) {
        errno = EBADF;
        return EPOLL_INVALID;
    }

    if (op != EPOLL_CTL_DEL && !event) {
        errno = EFAULT;
        return EPOLL_INVALID;
    }

    struct epoll_t *ep = (struct epoll_t *)efd;

    /* Take lock FIRST, then check closing — this prevents the
     * TOCTOU race where epoll_close could free ep between the
     * closing check and the internal lock acquisition in the
     * backend functions.  The locked variants assume lock held
     * and do NOT release it. */
    epoll_spinlock_lock(&ep->lock);

    int result;
    switch (op)
    {
        case EPOLL_CTL_ADD: result = ppoll_add_locked(ep, fd, event); break;
        case EPOLL_CTL_MOD: result = ppoll_mod_locked(ep, fd, event); break;
        case EPOLL_CTL_DEL: result = ppoll_del_locked(ep, fd); break;
        default:
            errno = EINVAL;
            result = EPOLL_INVALID;
            break;
    }

    epoll_spinlock_unlock(&ep->lock);
    return result;
}

int epoll_close(HANDLE efd)
{
    if (efd <= 0) {
        errno = EBADF;
        return EPOLL_INVALID;
    }

    struct epoll_t *ep = (struct epoll_t *)efd;

    epoll_spinlock_lock(&ep->lock);
    if (ep->closing) {
        epoll_spinlock_unlock(&ep->lock);
        errno = EBADF;
        return EPOLL_INVALID;
    }
    ep->closing = 1;
    /* Wake any poll() blocked in epoll_wait */
    write(ep->pipes[1], "x", 1);
    epoll_spinlock_unlock(&ep->lock);

    close(ep->pipes[0]); ep->pipes[0] = -1;
    close(ep->pipes[1]); ep->pipes[1] = -1;

    epoll_free(ep->entries);   ep->entries  = NULL;
    epoll_free(ep->pollfds);   ep->pollfds  = NULL;
#if EPOLL_USE_SHARED_WORKBUF
    epoll_free(ep->working);
#endif
    epoll_free(ep);

    return EPOLL_SUCCESS;
}

HANDLE epoll_create(int size)
{
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    return epoll_create1(0);
}

HANDLE epoll_create1(int flags)
{
    if (flags && flags != EPOLL_CLOEXEC) {
        errno = EINVAL;
        return -1;
    }
    errno = 0;

    struct epoll_t *ep = (struct epoll_t *)epoll_malloc(sizeof(struct epoll_t));
    if (!ep) {
        errno = ENOMEM;
        return -1;
    }
    memset(ep, 0, sizeof(struct epoll_t));

    ep->pipes[0] = ep->pipes[1] = -1;
    if (pipe(ep->pipes)) {
        epoll_free(ep);
        return -1;
    }

    if (flags & EPOLL_CLOEXEC) {
        epoll_nonexec(ep->pipes[0]);
        epoll_nonexec(ep->pipes[1]);
    }
    epoll_nonblock(ep->pipes[0]);
    epoll_nonblock(ep->pipes[1]);

    /* Allocate initial arrays */
    ep->capacity = 0;
    ep->entries  = NULL;
    ep->pollfds  = NULL;
    ep->free_head = -1;

    if (ppoll_grow(ep, PP_INITIAL_CAPACITY) != 0) {
        close(ep->pipes[0]);
        close(ep->pipes[1]);
        epoll_free(ep);
        return -1;
    }

    /* Remove slot 0 (pipe) from the free list — it's permanently
     * reserved.  free_head now points to slot 1, the first truly free slot. */
    ep->free_head = 1;

    /* Register the self-waking pipe read end at slot 0 — always present. */
    ep->entries[0].fd           = ep->pipes[0];
    ep->entries[0].epoll_events = EPOLLIN;
    ep->entries[0].data.ptr     = NULL;

    ep->pollfds[0].fd     = ep->pipes[0];
    ep->pollfds[0].events = POLLIN;
    ep->pollfds[0].revents = 0;

    ep->nfds = 1;
    ep->edited = false;
    epoll_spinlock_init(&ep->lock);

    return (HANDLE)ep;
}

int epoll_wait(HANDLE efd, struct epoll_event *events, int maxevents, int timeout)
{
    errno = 0;

    if (efd <= 0) {
        errno = EBADF;
        return EPOLL_INVALID;
    }

    if (!events) {
        errno = EFAULT;
        return EPOLL_INVALID;
    }

    if (maxevents <= 0) {
        errno = EINVAL;
        return EPOLL_INVALID;
    }

    struct epoll_t *ep = (struct epoll_t *)efd;

    /* ── Acquire working buffer ──
     *
     * Default (EPOLL_USE_SHARED_WORKBUF=0): per-call stack + heap.
     *   Stack for ≤ EPOLL_STACK_NFDS (zero alloc), heap fallback for
     *   larger sets.  Safe for concurrent epoll_wait on the same handle.
     *
     * Shared (EPOLL_USE_SHARED_WORKBUF=1): buffer lives on epoll_t;
     *   realloc when nfds exceeds current capacity.  NOT safe for
     *   concurrent epoll_wait on the same handle (undefined behavior). */
#if EPOLL_USE_SHARED_WORKBUF
    struct pollfd *working  = NULL;
    int64_t        poll_start = 0;
#else
    struct pollfd  wstack[EPOLL_STACK_NFDS];
    struct pollfd *working  = wstack;
    bool           heap     = false;
    size_t         wcap     = EPOLL_STACK_NFDS;
    int64_t        poll_start = 0;
#endif

    while (1)
    {
        /* ── Step 1: drain wake pipe and snapshot pollfds under lock ── */
        epoll_spinlock_lock(&ep->lock);
        ppoll_drain_pipe(ep);
        int nfds = ep->nfds;

        /* Grow working buffer on demand — only when nfds exceeds current
         * capacity.  Never shrinks, so retry loops pay zero alloc cost. */
#if EPOLL_USE_SHARED_WORKBUF
        if ((size_t)nfds > ep->wcap) {
            size_t newcap = round_up_pow2((size_t)nfds);
            if (newcap < EPOLL_INITIAL_CAP) newcap = EPOLL_INITIAL_CAP;
            struct pollfd *p = (struct pollfd *)epoll_realloc(ep->working,
                                    newcap * sizeof(struct pollfd));
            if (!p) {
                epoll_spinlock_unlock(&ep->lock);
                errno = ENOMEM;
                goto out;
            }
            ep->working = p;
            ep->wcap    = newcap;
        }
        working = ep->working;
#else
        if ((size_t)nfds > wcap) {
            size_t new_sz = (size_t)nfds * sizeof(struct pollfd);
            if (!heap) {
                working = (struct pollfd *)epoll_malloc(new_sz);
                heap = true;
            } else {
                working = (struct pollfd *)epoll_realloc(working, new_sz);
            }
            if (!working) {
                epoll_spinlock_unlock(&ep->lock);
                errno = ENOMEM;
                goto out;
            }
            wcap = (size_t)nfds;
        }
#endif

        memcpy(working, ep->pollfds, (size_t)nfds * sizeof(struct pollfd));
        epoll_spinlock_unlock(&ep->lock);

        /* ── Step 2: record start and call poll(2) ── */
        if (timeout > 0)
            poll_start = epoll_now_ms();

        int r = poll(working, (nfds_t)nfds, timeout);

        /* ── Check for concurrent epoll_close after poll returns ── */
        epoll_spinlock_lock(&ep->lock);
        if (ep->closing) {
            epoll_spinlock_unlock(&ep->lock);
#if !EPOLL_USE_SHARED_WORKBUF
            if (heap) { epoll_free(working); heap = false; }
#endif
            errno = EBADF;
            return EPOLL_INVALID;
        }

        if (r < 0) {
            epoll_spinlock_unlock(&ep->lock);
            /* EINTR is the only recoverable error — retry with decay. */
            if (errno == EINTR) {
                epoll_timeout_decay(&timeout, poll_start);
                continue;
            }
            goto out;
        }

        if (r == 0) {
            epoll_spinlock_unlock(&ep->lock);
            /* Timeout with no events */
#if !EPOLL_USE_SHARED_WORKBUF
            if (heap) { epoll_free(working); heap = false; working = wstack; }
#endif
            return 0;
        }

        /* ── Step 3: check for pipe wake ── */
        if (working[0].revents & POLLIN) {
            /* Another thread modified the set — drain and restart.
             * Decay remaining timeout if applicable. */
            epoll_spinlock_unlock(&ep->lock);
            epoll_timeout_decay(&timeout, poll_start);
            continue;
        }

        /* ── Step 4: translate poll revents → epoll events ──
         * Lock is still held from the closing check above, so no
         * concurrent epoll_close can free ep between the two. */
        /* epoll_spinlock_lock now redundant — already held */
        int nevents = 0;
        int remaining = r;
        for (int i = 1; i < nfds && nevents < maxevents && remaining > 0; i++)
        {
            short revents = working[i].revents;
            if (revents == 0)
                continue;

            remaining--;

            SOCKET fd = ep->entries[i].fd;

            /* Skip stale slot (freed and reused between snapshot and lock) */
            if (fd == -1 || fd != ep->pollfds[i].fd)
                continue;

            struct epoll_event *ev = &events[nevents];

            if (PP_HAS_ONESHOT(ep->entries[i].epoll_events)) {
                /* ONESHOT: report the event, then disable the entry so
                 * it won't fire again until EPOLL_CTL_MOD re-enables it. */
                ev->events   = PP_REVENTS2EPOLL(revents);
                ev->data     = ep->entries[i].data;

                ep->pollfds[i].events  = 0;
                ep->pollfds[i].revents = 0;

                nevents++;
                continue;
            }

            /* Normal (level-triggered) event */
            ev->events   = PP_REVENTS2EPOLL(revents);
            ev->data     = ep->entries[i].data;
            nevents++;
        }
        epoll_spinlock_unlock(&ep->lock);

#if !EPOLL_USE_SHARED_WORKBUF
        if (heap) epoll_free(working);
#endif
        return nevents;
    }

out:
#if !EPOLL_USE_SHARED_WORKBUF
    if (heap) epoll_free(working);
#endif
    return EPOLL_INVALID;
}
