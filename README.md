<div align="center">

# epoll

[![CI](https://github.com/CandyMi/epoll/workflows/CI/badge.svg)](https://github.com/CandyMi/epoll/actions)
[![License](https://img.shields.io/badge/license-MIT-informational)](LICENSE)
[![C](https://img.shields.io/badge/C-C99%20%2F%20C11-blue?logo=c)](https://en.cppreference.com/w/c)
[![Platform](https://img.shields.io/badge/platform-Windows%20|%20Linux%20|%20macOS%20|%20BSD%20|%20POSIX-blue)](AGENTS.md)

**Linux epoll ABI, everywhere.**

Write portable event-driven code using the familiar `epoll` API — on any platform.

`#include "epoll.h"` and use `epoll_create` / `epoll_ctl` / `epoll_wait` / `epoll_close` as you would on Linux. The library selects the optimal backend at build time:

| Platform | Backend | Mechanism |
|---|---|---|
| Linux / Android | `epoll.c` | Kernel `epoll_create1` / `epoll_ctl` / `epoll_wait` |
| macOS / BSD | `kepoll.c` | `kqueue` / `kevent` with same-fd event merging |
| Windows | `wepoll.c` | IOCP via vendored [piscisaureus/wepoll](https://github.com/piscisaureus/wepoll) |
| Other POSIX (recommended) | `pepoll.c` | `poll(2)` — unlimited fds, ms-direct timeout |
| Other POSIX (fallback) | `uepoll.c` | `select(2)` — limited to `FD_SETSIZE` (1024) |

</div>

---

## Features

- **ABI compatible** — same 6-function API (`epoll_create`, `epoll_create1`, `epoll_ctl`, `epoll_wait`, `epoll_close`, `epoll_allocator`)
- **Thread-safe** — spinlock-protected internal state (C11 `atomic_flag` or GCC `__sync_*`)
- **`EPOLLONESHOT`** — supported on all backends
- **`EPOLLET`** — supported on Linux native and kqueue (macOS/BSD) via `EV_CLEAR`
- **`EPOLLPRI`** — supported on Linux, macOS (kqueue `EVFILT_EXCEPT`), and poll (pepoll)
- **Custom allocator** — replace `malloc`/`realloc`/`free` via `epoll_allocator`
- **Concurrent-close safety** — `epoll_close` interrupts blocking wait and all subsequent operations return `EBADF`

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

## Build

### CMake (recommended)

```bash
cmake -S . -B build
cmake --build build
```

Options:

| Option | Default | Description |
|---|---|---|
| `-DEPFD_SELECT=ON` | OFF | Force `uepoll.c` (select) on non-Linux/BSD systems |
| `-DEPFD_POLL=ON` | ON (BSD) | Force `pepoll.c` (poll) on BSD/Darwin instead of kqueue |
| `-DENABLE_ASAN=ON` | OFF | Enable AddressSanitizer (GCC/Clang) |

### Manual

```bash
# Linux (kernel epoll)
gcc -I include/epoll src/epoll.c test/main.c -o main

# macOS/BSD (kqueue)
gcc -I include/epoll src/kepoll.c test/main.c -o main

# POSIX (poll — recommended fallback, unlimited fds)
gcc -I include/epoll src/pepoll.c test/main.c -o main

# POSIX (select — original fallback, limited to FD_SETSIZE)
gcc -I include/epoll src/uepoll.c test/main.c -o main

# Windows (IOCP, MSVC)
cl /I include/epoll src/wepoll.c test/main.c

# Windows (IOCP, MinGW)
gcc -I include/epoll src/wepoll.c test/main.c -o main.exe -lws2_32

# Force poll fallback on Linux (for testing)
gcc -DNO_NATIVE_EPOLL=1 -I include/epoll src/pepoll.c test/main.c -o main
```

---

## Backend Architecture

```mermaid
flowchart TD
    User["User Code"] -- #include "epoll.h" --> epoll_h["epoll.h (Dispatch)"]
    epoll_h -- "__linux__ / __ANDROID__" --> linux["epoll.c\nKernel epoll"]
    epoll_h -- "WIN32" --> win["wepoll.c\nIOCP + NT API"]
    epoll_h -- "other" --> uepoll_h
    uepoll_h -- "macOS / BSD" --> kqueue["kepoll.c\nkqueue / kevent"]
    uepoll_h -- "other POSIX\n(cmake selects)" --> poll["pepoll.c\npoll(2)"]
    uepoll_h -- "other POSIX\n(-DEPFD_SELECT=ON)" --> select["uepoll.c\nselect / fd_set"]
```

Only **one** `.c` file is compiled per platform. All backends implement the identical 6-function API.

---

## Thread Safety

| Operation | Safe to call concurrently? | Mechanism |
|---|---|---|
| `epoll_ctl` | ✅ Yes | Spinlock protects all internal data |
| `epoll_wait` | ✅ Yes | State snapshot taken under lock; system call uses per-thread output buffer (stack or per-call heap) — **all backends** |
| `epoll_close` | ✅ Best-effort | Sets `closing` flag under lock, interrupts blocking syscall, subsequent ops return `EBADF` |
| `epoll_create` / `epoll_create1` | ✅ Yes (handle not yet shared) | Single-threaded initialisation |
| `epoll_allocator` | ❌ No | Must be called once, before any other function |

### Spinlock

3-level degrading spinlock — no pthread dependency:

| Level | Condition | Implementation |
|---|---|---|
| 0 | `-DEPOLL_NO_THREADS` | No-op |
| 1 | C11 `<stdatomic.h>` | `atomic_flag` test-and-set |
| 2 | GCC `__sync_*` builtins | `__sync_lock_test_and_set` |
| — | None available | `#error` compilation failure |

---

## Platform Limitations

| Feature | Linux | macOS/BSD | Windows | POSIX (poll) | POSIX (select) |
|---|---|---|---|---|---|
| Max fds | Kernel limit | 4096 | System limit | RLIMIT_NOFILE | 1024 |
| `EPOLLET` | ✅ | ✅ (EV_CLEAR) | ❌ | ❌ | ❌ |
| `EPOLLHUP` | ✅ | ✅ | ❌ | ✅ (POLLHUP) | ❌ |
| `EPOLLRDHUP` | ✅ | ❌ | ❌ | ❌ | ❌ |
| `EPOLLPRI` | ✅ | ✅ macOS only | ❌ | ✅ (POLLPRI) | ❌ |
| `EPOLLONESHOT` | ✅ | ✅ | ✅ | ✅ | ✅ |
| Non-socket fds | ✅ | ✅ | ❌ | ✅ | ✅ |

See [docs/api.md](docs/api.md) for the complete API reference.

---

## License

MIT — see [LICENSE](LICENSE).

Author: [CandyMi](https://github.com/CandyMi)
