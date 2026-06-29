# epoll API Reference

## Overview

`epoll` is a cross-platform I/O event notification facility. This library emulates the Linux epoll ABI on Windows (IOCP), macOS/BSD (kqueue), and generic POSIX systems (select), allowing you to write portable event-driven code using a single API.

**Include:** `#include "epoll.h"` — no other setup required.

---

## Quick Start

```c
#include "epoll.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main() {
    int fds[2];
    pipe(fds);

    HANDLE efd = epoll_create(1);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fds[0]};
    epoll_ctl(efd, EPOLL_CTL_ADD, fds[0], &ev);

    write(fds[1], "hello", 5);

    struct epoll_event out;
    int n = epoll_wait(efd, &out, 1, -1);
    if (n > 0) {
        char buf[64];
        read(out.data.fd, buf, sizeof(buf));
        printf("received: %s\n", buf);
    }

    epoll_close(efd);
    close(fds[0]);
    close(fds[1]);
    return 0;
}
```

---

## API Functions

### `epoll_allocator`

```c
typedef void* (*epoll_realloc_t)(void* ptr, size_t size);
void epoll_allocator(epoll_realloc_t alloc_func);
```

Replace the internal memory allocator.

| Parameter | Description |
|---|---|
| `alloc_func` | Allocation function with `realloc` semantics, or `NULL` to keep current |

**Notes:**
- Default allocator is the standard `realloc`.
- Must be called **before** any other epoll function. Not thread-safe — do not call concurrently with any other epoll operation.
- On Linux (`epoll.c`), this is a no-op because the kernel backend performs no user-space allocations.

---

### `epoll_create`

```c
HANDLE epoll_create(int size);
```

Create a new epoll instance.

| Parameter | Description |
|---|---|
| `size` | Ignored (must be ≥ 0). **Linux kernel rejects `size == 0`** with `EINVAL` — use `epoll_create(1)` or larger for portable code |

| Return | Description |
|---|---|
| `> 0` | Epoll handle on success |
| `HANDLE(-1)` | Error, `errno` set |

| `errno` | Condition |
|---|---|
| `EINVAL` | `size < 0`, or `size == 0` on Linux kernel |
| `ENOMEM` | Out of memory |
| (system) | See `epoll_create1` for underlying failures |

---

### `epoll_create1`

```c
HANDLE epoll_create1(int flags);
```

Create a new epoll instance with flags.

| Parameter | Description |
|---|---|
| `flags` | `0` or `EPOLL_CLOEXEC` |

| Return | Description |
|---|---|
| `> 0` | Epoll handle on success |
| `HANDLE(-1)` | Error, `errno` set |

| `errno` | Condition |
|---|---|
| `ENOMEM` | Out of memory |
| (system) | Backend-specific: `kqueue()`, `pipe()`, or `malloc` failure |

**Flags:**

| Flag | Value | Effect |
|---|---|---|
| `EPOLL_CLOEXEC` | `1` | Set `FD_CLOEXEC` on all internal file descriptors |

**Platform notes:**
- **Linux:** delegates to `epoll_create1(flags)`.
- **kqueue:** sets `FD_CLOEXEC` via `fcntl()` after `kqueue()`.
- **select:** sets `FD_CLOEXEC` via `fcntl()` on the self-waking pipe fds.
- **Windows (wepoll):** `EPOLL_CLOEXEC` is ignored (no concept on Windows).

---

### `epoll_close`

```c
int epoll_close(HANDLE efd);
```

Close an epoll instance and release all resources.

| Parameter | Description |
|---|---|
| `efd` | Epoll handle returned by `epoll_create` or `epoll_create1` |

| Return | Description |
|---|---|
| `0` | Success |
| `-1` | Error, `errno` set |

| `errno` | Condition |
|---|---|
| `EBADF` | Invalid handle (`efd ≤ 0`) |
| (system) | Underlying `close()` failure |

**Concurrency safety (since v2.0):**
`epoll_close` sets an internal `closing` flag under the spinlock, then:
- **kqueue backend:** closes the kqueue fd — this interrupts any concurrent `kevent()` blocking in another thread. The interrupted `epoll_wait` detects the `closing` flag on return and exits with `EBADF`.
- **select/poll backends:** writes to the self-waking pipe — this interrupts any concurrent `select()`/`poll()`. The interrupted `epoll_wait` detects the `closing` flag and exits with `EBADF`.
- All subsequent `epoll_ctl`/`epoll_wait` calls on the same handle also return `EBADF` immediately.

**⚠️ Best practice:** While the library now handles concurrent `epoll_close` safely, production code should still ensure only one thread calls `epoll_close` per handle — the closing-flag mechanism is a safety net, not a license to race.

**Platform notes:**
- **Linux:** calls `close(efd)` directly. `errno` may reflect `close(2)` errors.
- **kqueue:** closes the kqueue fd then frees the `epoll_t` struct. Returns `-1` if `efd ≤ 0` or the internal fd is invalid.
- **select:** closes both ends of the self-waking pipe then frees the `epoll_t` struct.
- **poll (pepoll):** closes both ends of the self-waking pipe, frees the dynamic entries/pollfds arrays, then frees the `epoll_t` struct.
- **Windows (wepoll):** closes the underlying IOCP port.

---

### `epoll_ctl`

```c
int epoll_ctl(HANDLE efd, int op, SOCKET fd, struct epoll_event* event);
```

Control interface for an epoll instance — register, modify, or remove a file descriptor.

| Parameter | Description |
|---|---|
| `efd` | Epoll handle |
| `op` | Operation: `EPOLL_CTL_ADD`, `EPOLL_CTL_MOD`, or `EPOLL_CTL_DEL` |
| `fd` | Target file descriptor |
| `event` | Event struct (ignored for `EPOLL_CTL_DEL`) |

| Return | Description |
|---|---|
| `0` | Success |
| `-1` | Error, `errno` set |

| `errno` | Condition |
|---|---|
| `EBADF` | Invalid epoll handle |
| `EEXIST` | `EPOLL_CTL_ADD` on an already-registered fd |
| `ENOENT` | `EPOLL_CTL_MOD`/`EPOLL_CTL_DEL` on an unregistered fd |
| `EINVAL` | Invalid `op` value |
| `ENOSPC` | fd out of range (**select backend only**: `fd ≥ FD_SETSIZE`) |
| `EPERM` | Unsupported fd type (**select backend only**) |
| `ENOMEM` | Out of memory |

**Operations:**

| Op | Value | Description |
|---|---|---|
| `EPOLL_CTL_ADD` | `1` | Register `fd` with the epoll instance |
| `EPOLL_CTL_MOD` | `2` | Change the events associated with `fd` |
| `EPOLL_CTL_DEL` | `3` | Remove `fd` from the epoll instance |

**Platform notes:**
- **select backend:** Validates `fd` type via `fstat()` — only sockets, FIFOs, character devices, and regular files are accepted. `fd` must be in range `[0, FD_SETSIZE)` (default 1024).
- **Windows (wepoll):** Only sockets and named pipes are supported. Passing a regular file will fail.

---

### `epoll_wait`

```c
int epoll_wait(HANDLE efd, struct epoll_event* events, int maxevents, int timeout);
```

Wait for events on the epoll instance.

| Parameter | Description |
|---|---|
| `efd` | Epoll handle |
| `events` | Output array for ready events |
| `maxevents` | Maximum number of events to return (must be > 0) |
| `timeout` | Timeout in milliseconds; `-1` = infinite, `0` = return immediately |

| Return | Description |
|---|---|
| `> 0` | Number of ready events |
| `0` | Timeout expired with no events |
| `-1` | Error, `errno` set |

| `errno` | Condition |
|---|---|
| `EBADF` | Invalid epoll handle |
| `EFAULT` | `events` is `NULL` (see note below) |
| `EINVAL` | `maxevents ≤ 0` |
| (system) | Backend-specific `select()`/`kevent()`/`GetQueuedCompletionStatus()` error |

**⚠️ `EFAULT` note for Linux kernel:** The kernel only validates the `events` pointer when events are actually pending. If no events are ready and `timeout=0`, `epoll_wait` returns `0` immediately **without** checking the pointer — so `EFAULT` is **not** guaranteed. Our non-Linux backends always check `events != NULL` upfront. For portable code, never rely on `EFAULT` being returned for NULL events.

**Timeout behavior:**
- `timeout = -1`: Block indefinitely until at least one event is available.
- `timeout = 0`: Poll — return immediately even if no events are ready.
- `timeout > 0`: Block for up to `timeout` milliseconds.

**Platform notes:**
- **kqueue backend:** `maxevents` is internally capped at `EPOLL_MAX_EVENTS` (4096). Uses a per-call kevent buffer (stack ≤ 256 events, heap fallback) — multiple threads can call `epoll_wait` concurrently on the same instance.
- **select backend:** Uses per-call stack copies of `fd_set` — multiple threads can call `epoll_wait` concurrently. If the self-waking pipe triggers (from a concurrent `epoll_ctl` call), `epoll_wait` recalculates the remaining timeout and retries.
- **poll backend (pepoll):** Uses per-call stack (≤256 fds) or per-call heap working buffer — multiple threads can call `epoll_wait` concurrently.

---

## Data Types

### `HANDLE`

```c
// Linux:   typedef int HANDLE;
// Windows: typedef void* HANDLE;
// Other:   typedef intptr_t HANDLE;
```

Opaque handle to an epoll instance. Never inspect or modify its value — pass only to the epoll API functions.

### `SOCKET`

```c
// Linux:       typedef int SOCKET;
// Windows:     typedef uintptr_t SOCKET;
// Other:       typedef int SOCKET;
```

File descriptor or socket handle.

### `epoll_data_t`

```c
typedef union epoll_data_t {
    void*   ptr;
    int     fd;
    uint32_t u32;
    uint64_t u64;
    SOCKET  sock;   /* Windows specific */
    HANDLE  hnd;    /* Windows specific */
} epoll_data_t;
```

User-data union carried with each registered fd. The value set in `epoll_ctl` is returned unchanged by `epoll_wait`.

### `epoll_event`

```c
typedef struct epoll_event {
    uint32_t     events;  /* bitmask of EPOLL_* flags */
    int32_t      _nouse;  /* internal — do not touch */
    epoll_data_t data;    /* user data */
} epoll_event;
```

**⚠️ Important:** On the **select backend** (`uepoll.h`), `epoll_event` has an internal `_nouse` field before `events`. On **Windows** (`wepoll.h`), it does not. Binary sharing of event structs between backends is unsafe.

---

## Event Flags

| Flag | Value | Description | Backend support |
|---|---|---|---|
| `EPOLLIN` | `0x001` | Data available to read | All |
| `EPOLLOUT` | `0x004` | Ready for writing | All |
| `EPOLLERR` | `0x008` | Error condition on fd | All |
| `EPOLLHUP` | `0x010` | Hang up (peer closed) | **kqueue + Linux**; select doesn't emulate this |
| `EPOLLRDHUP` | `0x2000` | Peer closed connection (TCP half-close) | **Linux only** |
| `EPOLLPRI` | `0x002` | Urgent data available | Linux, macOS (kqueue EVFILT_EXCEPT) |
| `EPOLLRDNORM` | `0x040` | Normal data readable (alias for `EPOLLIN`) | All |
| `EPOLLRDBAND` | `0x080` | Priority band data readable | All |
| `EPOLLWRNORM` | `0x100` | Normal data writable (alias for `EPOLLOUT`) | All |
| `EPOLLWRBAND` | `0x200` | Priority band data writable | All |
| `EPOLLMSG` | `0x400` | Never reported (SELinux Sun stream) | All (defined only) |
| `EPOLLET` | `0x80000000` | Edge-triggered notification | **kqueue only**; unsupported on select backend |
| `EPOLLONESHOT` | `0x40000000` | One-shot notification (auto-disable after first event) | All |
| `EPOLLEXCLUSIVE` | `0x10000000` | Wake only one thread (Linux 4.5+) | Defined for API compatibility only |
| `EPOLLWAKEUP` | `0x20000000` | System wake-up (Android) | Defined for API compatibility only |

---

## Level-Triggered (LT) vs Edge-Triggered (ET)

### Level-Triggered (LT) — default

When a fd is registered without `EPOLLET`, it behaves like `poll()`:

- `epoll_wait` returns the fd as long as the condition is true (data available, space for write).
- If you read only part of the data, the **same event is returned again** on the next `epoll_wait`.
- Safe for blocking I/O — no risk of starvation.

### Edge-Triggered (ET)

When `EPOLLET` is specified, events are delivered only on **state changes**:

- `epoll_wait` returns the fd once when data arrives, even if you don't read all of it.
- Subsequent calls to `epoll_wait` **may block** even if unread data remains in the buffer.
- **Requires non-blocking fds** — loop on `read(2)`/`write(2)` until `EAGAIN`, then call `epoll_wait` again.

```
Example (pipe with 2 KB written, 1 KB read):

  LT:          write 2KB → wait → read 1KB → wait → returns immediately (1KB left)
  ET:          write 2KB → wait → read 1KB → wait → may block (no new data arrived)
```

**Usage pattern for ET:**

```c
// 1. Set fd non-blocking
fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

// 2. Register with EPOLLET
struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.fd = fd};
epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);

// 3. Event loop: read until EAGAIN
while (1) {
    struct epoll_event out;
    epoll_wait(efd, &out, 1, -1);
    char buf[4096];
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == -1 && errno == EAGAIN) break;  // all consumed
        if (n <= 0) break;                       // error or EOF
        // process buf[0..n)
    }
}
```

### ET + ONESHOT

Combining `EPOLLET | EPOLLONESHOT` delivers at most one event per fd. After the event is consumed, the fd is automatically disabled. Re-arm with `epoll_ctl(efd, EPOLL_CTL_MOD, fd, &ev)`.

### Multi-threaded ET

When multiple threads call `epoll_wait` on the same instance, and an ET fd becomes ready, **only one thread** is woken — avoiding the "thundering herd" problem.

---

## Thread Safety

- `epoll_ctl` and `epoll_wait` are **thread-safe** on the same epoll instance. Internal data is protected by a spinlock (see below) and all blocking system calls use per-thread output buffers.
- `epoll_close` is **best-effort safe** — it sets an internal `closing` flag under the spinlock, interrupts any blocking system call in another thread (via kqueue fd close or self-waking pipe), and subsequent operations on the handle return `EBADF` immediately.
- `epoll_create`/`epoll_create1` should not be called concurrently with other operations on the same instance (there's nothing to race on — the handle isn't published yet).
- `epoll_allocator` must be called **before** any other function and must **not** be called concurrently with any other epoll operation.

### Spinlock Implementation

The library uses a 3-level degrading spinlock:

| Level | Condition | Mechanism |
|---|---|---|
| 0 | `EPOLL_NO_THREADS` defined | No-op (single-threaded assumed) |
| 1 | C11 `<stdatomic.h>` available | `atomic_flag` test-and-set |
| 2 | GCC `__sync_*` builtins available | `__sync_lock_test_and_set` |
| — | None of the above | **Compile error** (`#error`) |

No pthread or OS-level mutex is used. Spinlocks are suitable because critical sections are short (registering a kevent or copying an fd_set).

---

## Error Handling

All functions return `-1` on error and set `errno` appropriately. Standard errno values:

| `errno` | Meaning |
|---|---|
| `EBADF` | Invalid epoll handle |
| `EINVAL` | Invalid argument (op, size, maxevents, etc.) |
| `ENOENT` | fd not registered (MOD/DEL without ADD) |
| `EEXIST` | fd already registered (duplicate ADD) |
| `ENOMEM` | Memory allocation failure |
| `EPERM` | Unsupported fd type (select backend) |
| `ENOSPC` | fd ≥ FD_SETSIZE (select backend) |
| `EFAULT` | NULL event/output pointer |

---

## Platform-Specific Limitations

| Feature | Linux | macOS/BSD (kqueue) | Windows (wepoll) | POSIX (select/uepoll) | POSIX (poll/pepoll) |
|---|---|---|---|---|---|
| Max fds | Kernel limit | 4096 (`EPOLL_MAX_EVENTS`) | System limit | 1024 (`FD_SETSIZE`) | **unlimited** (RLIMIT_NOFILE) |
| `EPOLLET` | ✅ | ✅ (→ `EV_CLEAR`) | ❌ | ❌ | ❌ |
| `EPOLLHUP` | ✅ | ✅ (EV_EOF → `EPOLLHUP`) | ❌ | ❌ | ✅ (via POLLHUP) |
| `EPOLLRDHUP` | ✅ | ❌ | ❌ | ❌ | ❌ |
| `EPOLLPRI` | ✅ | ✅ macOS only (EVFILT_EXCEPT) | ❌ | ❌ | ✅ (via POLLPRI) |
| `EPOLLONESHOT` | ✅ | ✅ (→ `EV_ONESHOT`) | ✅ | ✅ | ✅ |
| Non-socket fds | ✅ | ✅ | ❌ (sockets/pipes only) | ✅ | ✅ |
| `epoll_event._nouse` | N/A | N/A | ❌ (no such field) | ✅ (internal) | ✅ (internal) |
| Concurrent epoll_wait | ✅ (kernel) | ✅ (per-call buffer) | ✅ (IOCP) | ✅ (per-call fd_set) | ✅ (per-call buffer) |
| epoll_close safe | N/A (kernel) | ✅ (closing flag) | ✅ (closing flag) | ✅ (closing flag) | ✅ (closing flag) |
| Timeout precision | ms | ms (timespec) | ms | ms (timeval) | **ms directly** (poll) |
| Self-waking | kernel | N/A (close-fd interrupt) | N/A | pipe | pipe |

---

## Building

```bash
cmake -S . -B build
cmake --build build
```

Or compile directly:

```bash
# Linux (kernel epoll)
gcc -I include/epoll src/epoll.c test/main.c -o main

# Linux (force poll fallback for testing)
gcc -DNO_NATIVE_EPOLL=1 -I include/epoll src/pepoll.c test/main.c -o main

# Linux (force select fallback for testing)
gcc -DNO_NATIVE_EPOLL=1 -I include/epoll src/uepoll.c test/main.c -o main

# macOS/BSD (kqueue)
gcc -I include/epoll src/kepoll.c test/main.c -o main

# POSIX fallback (select) — limited to FD_SETSIZE
gcc -I include/epoll src/uepoll.c test/main.c -o main

# POSIX fallback (poll) — unlimited fds, no FD_SETSIZE limit
gcc -I include/epoll src/pepoll.c test/main.c -o main

# macOS/BSD (force poll instead of kqueue via cmake)
cmake -S . -B build -DEPFD_POLL=ON
cmake --build build

# Windows (IOCP, MSVC)
cl /I include/epoll src/wepoll.c test/main.c

# Windows (IOCP, MinGW)
gcc -I include/epoll src/wepoll.c test/main.c -o main.exe -lws2_32

# Enable AddressSanitizer for debugging
cmake -S . -B build -DENABLE_ASAN=ON
cmake --build build
```
