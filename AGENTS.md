# AGENTS.md — epoll Cross-Platform Compatibility Layer

## Project Overview

This is a **Linux epoll ABI compatibility layer** for cross-platform event-driven programming. Users simply `#include "epoll.h"` and write to the epoll API on Linux, Windows, macOS/FreeBSD, and any other POSIX system.

**Author:** CandyMi — [github.com/CandyMi](https://github.com/CandyMi)  
**License:** MIT

## Architecture

```mermaid
flowchart TD
    User["User Code"] -- #include "epoll.h" --> epoll_h["epoll.h (Dispatch)"]
    epoll_h -- "__linux__ / __ANDROID__\n(NO_NATIVE_EPOLL=0)" --> linux["epoll.c\nKernel epoll"]
    epoll_h -- "Linux + -DNO_NATIVE_EPOLL=1" --> uepoll_h
    epoll_h -- WIN32 --> win["wepoll.c\nIOCP + NT API"]
    epoll_h -- other --> uepoll_h["uepoll.h (types + API)"]
    uepoll_h -- "macOS / BSD\n(-DEPFD_POLL=OFF)" --> kqueue["kepoll.c\nkqueue / kevent"]
    uepoll_h -- "BSD/Darwin default\n(EPFD_POLL=ON)" --> poll["pepoll.c\npoll(2)"]
    uepoll_h -- "other POSIX default" --> poll
    uepoll_h -- "other POSIX\n(-DEPFD_SELECT=ON)" --> select["uepoll.c\nselect / fd_set"]
```

**Dispatch logic** in [`include/epoll/epoll.h`](include/epoll/epoll.h):
- `__linux__` / `__ANDROID__` + `NO_NATIVE_EPOLL=0` (default) → includes `<sys/epoll.h>` directly, defines thin shim functions
- `__linux__` + `-DNO_NATIVE_EPOLL=1` → includes `uepoll.h` (for CI/testing)
- `WIN32` → delegates to `wepoll.h`
- All others → delegates to `uepoll.h` (shared types header for all non-Linux backends)

**The actual backend selection is done by CMake at build time** ([`CMakeLists.txt:15-38`](CMakeLists.txt:15)): only one `.c` file is compiled per platform. All backends share the same 6-function API signature.

**Note on BSD/Darwin defaults:** `EPFD_POLL` defaults to `ON`, meaning the default cmake build on macOS/BSD compiles `pepoll.c` (poll), not `kepoll.c` (kqueue). Pass `-DEPFD_POLL=OFF` to use the kqueue backend.

## File Inventory

```
epoll/
├── CMakeLists.txt              Build system (OS-conditional compilation + install rules)
├── README.md                   User documentation (platforms, features, usage, architecture)
├── LICENSE                     MIT License
├── AGENTS.md                   Project reference (this file)
├── include/
│   └── epoll/
│       ├── epoll.h   (53 L)    Dispatch header — #if/#elif/#else selects backend compile-time;
│       │                         respects -DNO_NATIVE_EPOLL=1 to force uepoll on Linux
│       ├── uepoll.h  (115 L)   Non-Linux API types/constants/signatures (HANDLE=intptr_t)
│       └── wepoll.h  (119 L)   Windows API definitions (HANDLE=void*, SOCKET=uintptr_t)
├── src/
│   ├── epoll.c     (25 L)      Linux native shim — no-op allocator + close(2) wrapper
│   ├── kepoll.c    (406 L)     macOS/BSD kqueue backend — struct epoll_t {int efd; lock;
│   │                         closing; kqevents[] heap buffer}
│   ├── pepoll.c    (722 L)     POSIX poll(2) backend — dynamic pollfd array with free-list,
│   │                         no FD_SETSIZE limit (RLIMIT_NOFILE)
│   ├── uepoll.c    (544 L)     POSIX select fallback — self-waking pipe + fd_set[3]
│   └── wepoll.c    (2195 L)    Windows IOCP backend (vendored piscisaureus/wepoll)
├── test/
│   └── main.c      (564 L)     19–20 test cases via EPOLL_TEST_FUNCTION macro;
│                                 guarded for WIN32 / uses epoll_create(1)
└── docs/
    └── api.md                  Full API reference (functions, flags, ET/LT, examples, build,
                                  thread-safety closing flag)
```

## Public API (6 functions, uniform across all backends)

| Function | Signature Location | Semantics |
|---|---|---|
| `epoll_create(int size)` | [`uepoll.h:91`](uepoll.h:91) / [`wepoll.h:101`](wepoll.h:101) | Create epoll instance; `size` is ignored (matches Linux 2.6.8+ behavior) |
| `epoll_create1(int flags)` | [`uepoll.h:93`](uepoll.h:93) / [`wepoll.h:102`](wepoll.h:102) | Supports `EPOLL_CLOEXEC` |
| `epoll_ctl(HANDLE, int op, SOCKET, struct epoll_event*)` | [`uepoll.h:97`](uepoll.h:97) / [`wepoll.h:106`](wepoll.h:106) | ADD / MOD / DEL operations |
| `epoll_wait(HANDLE, struct epoll_event*, int maxevents, int timeout)` | [`uepoll.h:99`](uepoll.h:99) / [`wepoll.h:111`](wepoll.h:111) | Wait for events; `timeout=-1` blocks indefinitely |
| `epoll_close(HANDLE)` | [`uepoll.h:95`](uepoll.h:95) / [`wepoll.h:104`](wepoll.h:104) | Release all resources; sets closing flag, interrupts any blocking wait |
| `epoll_allocator(epoll_realloc_t)` | [`uepoll.h:89`](uepoll.h:89) / [`wepoll.h:100`](wepoll.h:100) | Replace internal `malloc`/`realloc`/`free` |

## Backend-Specific Design Decisions

### epoll.c — Linux Native Shim

- `epoll_allocator` is a no-op (`(void)alloc`) — no internal allocation on Linux.
- `epoll_close` directly calls `close(efd)`.

### kepoll.c — kqueue Backend

- **HANDLE is a pointer**: `struct epoll_t` wraps a kqueue fd + spinlock + `volatile int closing` + heap-allocated `kqevents[]` buffer. `epoll_create1` pre-allocates the kevent buffer (sized `EPOLL_MAX_EVENTS = 4096`) to avoid `alloca()` on every `epoll_wait`. On OOM, the kqueue fd is closed and the allocation is freed.
- **Event mapping**: `EPOLLIN|RDNORM|RDBAND|ERR|HUP → EVFILT_READ`, `EPOLLOUT|WRNORM|WRBAND → EVFILT_WRITE`. Non-requested filters are registered as `EV_DISABLE`. `EPOLLERR`/`EPOLLHUP` also enable `EVFILT_READ` so EOF/error can be detected.
- **EPOLLPRI support**: `EPOLLPRI → EVFILT_EXCEPT | NOTE_OOB` via a separate `kevent()` call (isolated from pipe-unsupported `EINVAL`). Guarded by `#if defined(EVFILT_EXCEPT) && defined(NOTE_OOB)` — only available on macOS; FreeBSD and other BSDs skip EPOLLPRI.
- **Event merging**: kqueue returns separate READ/WRITE events for the same fd. A `uintptr_t before_fd` cache merges adjacent same-fd events into a single `epoll_event`. ERROR/EOF/ONESHOT events are handled separately and counted in the return value.
- **ET mode**: `EPOLLET → EV_CLEAR` flag on kevent.
- **ONESHOT handling**: After returning from `epoll_wait`, `EV_ONESHOT` events are first written to the user `events[]` buffer, then `kepoll_del(ep, fd, false)` is called (DISABLE only, not DELETE).
- **NULL event validation**: `_kepoll_register` checks for NULL `event` and returns `EFAULT` before taking the spinlock.
- **Thread safety**: 3-level degrading spinlock (C11 `atomic_flag` → GCC `__sync_*` → `#error`). No pthread fallback.
- **Concurrent close**: `epoll_close` sets `closing` flag under lock and `close(efd)` to interrupt any concurrent `kevent()`. `epoll_wait` checks `closing` after kevent returns.
- **Hardcoded maxevents**: `#define EPOLL_MAX_EVENTS 4096`.

### pepoll.c — poll(2) Backend (recommended POSIX fallback)

- **Core data structure**: `struct epoll_t` contains `pipes[2]` (self-waking) + spinlock + `volatile int closing` + dynamic arrays (`pollfd *pollfds`, `struct poll_entry *entries`) with a free-list for O(1) slot allocation.
- **No FD_SETSIZE limit**: Uses dynamic arrays that grow on demand (initial 64, max 256K). Upper bound is `RLIMIT_NOFILE`.
- **Free-list**: Free slots are singly-linked via `entries[].epoll_events` (overloading the uint32 field — no extra memory cost). Slot 0 is permanently reserved for the self-waking pipe.
- **Working buffer**: Per-call stack or heap. ≤256 fds uses stack (`wstack[WO_STACK_NFDS]`); larger sets heap-allocate via `epoll_malloc`/`epoll_realloc`. Freed at all return paths (timeout, events, error) to prevent leaks.
- **Self-waking pipe**: `ppoll_drain_pipe()` clears the wake pipe under spinlock. `ppoll_register_locked`/`ppoll_unregister_locked` write `"x"` to wake any blocked `poll()`.
- **ONESHOT**: After event delivery, sets `pollfds[i].events = 0` to disable the entry. Re-arm via `EPOLL_CTL_MOD`.
- **Event mapping**: Dedicated macros `PP_EPOLL2POLL` (epoll→poll) and `PP_REVENTS2EPOLL` (poll→epoll). `POLLNVAL → EPOLLERR`, `POLLHUP → EPOLLHUP`, `POLLPRI → EPOLLPRI`. Poll has no concept of ET — always level-triggered.
- **Concurrent close**: `epoll_close` sets `closing` under lock and writes to the wake pipe. `epoll_wait` checks `closing` after `poll()` returns. All internal add/mod/del check `closing` after taking the spinlock.
- **Timeout recalculation**: After pipe wake or `EINTR`, `ppoll_now_ms()` (using `CLOCK_MONOTONIC_COARSE` or fallback) recalculates remaining ms.

### uepoll.c — select(2) Backend (original POSIX fallback)

- **Core data structure**: `struct epoll_t` contains `nfds` + `pipes[2]` (self-waking pipe) + `epoll_lock_t` + `volatile int closing` + three `fd_set`s + `epoll_event udata[FD_SETSIZE]`.
- **fd = array index**: `udata[fd]` — this is why fd upper bound = `FD_SETSIZE` (default 1024).
- **Self-waking mechanism**: `epoll_notice` macro writes `"x"` (1 byte) to `pipes[1]` after `epoll_ctl` modifies the fd sets, waking any `epoll_wait` blocked in `select()`. `epoll_receive` drains the pipe with a 1024-byte buffer in a `while` loop (fully clears the pipe to prevent spurious wake-ups under heavy concurrent modification). The pipe read end (`pipes[0]`) is registered into the epoll instance itself during `epoll_create1`, and `epoll_wait` detects the wake via `FD_ISSET(ep->pipes[0], &rset)`.
- **Thread safety 3-level degrading**: Same pattern as other backends.
- **ONESHOT handling**: After `epoll_wait` returns, the fd is removed from all `fd_set`s. The `_nouse` field is set to 1 (marking the slot as unregistered).
- **fd type validation**: `uepoll_can_poll` only allows sockets, FIFOs, character devices, and regular files.
- **EPOLLET unsupported**: The select model cannot simulate edge-triggered behavior.
- **Timeout recalculation**: After a self-wake pipe interruption, `CLOCK_MONOTONIC_COARSE` (or fallbacks) recalculates remaining timeout.
- **Concurrent close**: Same pattern as pepoll.c — closing flag under lock, wake pipe interrupts select, all internal functions check closing.

### wepoll.c — Windows IOCP Backend

- Third-party library ([piscisaureus/wepoll](https://github.com/piscisaureus/wepoll)); not directly maintained here.
- Only supports `SOCKET`/`Pipe` — regular files are unsupported.
- `HANDLE = void*`, `SOCKET = uintptr_t`.
- `epoll_event` has **no `_nouse` field**, unlike the uepoll variant.
- `epoll_allocator` is a no-op — wepoll uses `malloc`/`free` directly. Substituting the allocator has no effect on this backend.

## Supported Platforms & Compilers

| Platform | CMake Match | Backend (default) | Mechanism | Override |
|---|---|---|---|---|
| **Linux** | `Linux` | `epoll.c` | Kernel epoll | `-DNO_NATIVE_EPOLL=1` forces uepoll |
| **Android** | `Android` | `epoll.c` | Kernel epoll | same |
| **Windows** | `Windows` | `wepoll.c` | IOCP + NT API | — |
| **macOS / BSD** | `Darwin` / `BSD` | `pepoll.c` (`EPFD_POLL=ON` by default) | poll(2) | `-DEPFD_POLL=OFF` → kepoll.c (kqueue) |
| **Other POSIX** | else | `pepoll.c` | poll(2) | `-DEPFD_SELECT=ON` → uepoll.c (select) |

**Compilers:** MSVC / GCC / Clang  
**C Standard:** Not forced; modern compilers default to C11/C17. Minimum requirement is C99 (`for`-loop declarations, `<stdbool.h>`, `//` comments). C11 atomics auto-detected via `__STDC_VERSION__ >= 201112L`, with GCC `__sync_*` fallback.  
**Architecture:** 32/64-bit. `HANDLE` adapts to pointer width (`intptr_t` / `void*`). No inline assembly or endianness assumptions.

## Build & Test

### Build

```bash
cmake -S . -B build
cmake --build build
```

CMake selects the backend by `CMAKE_SYSTEM_NAME` ([`CMakeLists.txt`](CMakeLists.txt)):
- Windows → `src/wepoll.c`
- Linux/Android → `src/epoll.c`
- BSD/Darwin → `src/pepoll.c` (since `EPFD_POLL=ON` by default; pass `-DEPFD_POLL=OFF` for kqueue)
- Other → `src/pepoll.c` (pass `-DEPFD_SELECT=ON` for select/uepoll)

**Build artifacts:** shared library (`libepoll.so`/`.dylib`/`.dll`) + static library (`libepoll.a`/`.lib`) + test executable (`build/main`). Windows: static-only (no `.dll`).

**AddressSanitizer** (GCC/Clang, debug builds):
```bash
cmake -S . -B build_asan -DENABLE_ASAN=ON
cmake --build build_asan
./build_asan/main
```

**Force poll/select fallback on Linux** (for testing):
```bash
# poll (recommended)
gcc -DNO_NATIVE_EPOLL=1 -I include/epoll src/pepoll.c test/main.c -o main

# select (original fallback)
gcc -DNO_NATIVE_EPOLL=1 -I include/epoll src/uepoll.c test/main.c -o main
```

**Install:**
```bash
cmake --install build --prefix /usr/local
```
Installs headers to `${CMAKE_INSTALL_INCLUDEDIR}/epoll/` and libraries to `${CMAKE_INSTALL_LIBDIR}`.

### Test

```bash
./build/main
```

### CI (GitHub Actions)

`.github/workflows/ci.yml` builds and tests all five backends × compilers on every push/PR:

| Job | OS | Backend | Compiler |
|---|---|---|---|
| `linux-gcc` | ubuntu-latest | `src/epoll.c` (kernel epoll) | GCC |
| `linux-clang` | ubuntu-latest | `src/epoll.c` (kernel epoll) | Clang |
| `linux-select` | ubuntu-latest | `src/uepoll.c` (select fallback) | GCC |
| `linux-pepoll` | ubuntu-latest | `src/pepoll.c` (poll fallback) | GCC |
| `macos-clang` | macos-latest | `src/pepoll.c` (poll; EPFD_POLL=ON default) | Clang |
| `windows-msvc` | windows-latest | `src/wepoll.c` (IOCP) | MSVC |
| `windows-clang` | windows-latest | `src/wepoll.c` (IOCP) | ClangCL |
| `windows-mingw` | windows-latest | `src/wepoll.c` (IOCP) | MinGW-w64 (GCC) |

19–20 test functions registered in `main()` (20 defined, ET mode only runs on kqueue builds with `EPFD_POLL=OFF`):

1. `testcase_epoll_timer` — epoll_wait timeout accuracy
2. `testcase_epoll_alloc` — rapid create/close (100k iterations)
3. `testcase_epoll_watch` — basic EPOLLIN watch on pipe
4. `testcase_epoll_repeat` — 100k repeated epoll_wait with persistent data
5. `testcase_epoll_oneshot` — EPOLLONESHOT disables after first trigger (also verifies events field has EPOLLIN)
6. `testcase_epoll_et_mode` — EPOLLET: only compiled and run on kqueue (BSD/Darwin with `EPFD_POLL=OFF`)
7. `testcase_epoll_ctl_del` — delete then verify fd no longer watched
8. `testcase_epoll_ctl_mod` — modify data.ptr mid-stream
9. `testcase_epoll_out` — EPOLLOUT on writable pipe
10. `testcase_epoll_multi_fd` — two pipes both readable
11. `testcase_epoll_ctl_error` — invalid arguments to epoll_ctl/wait
12. `testcase_epoll_hup` — close peer, verify epoll_close works
13. `testcase_epoll_hup_flag` — pipe close → verify EPOLLHUP set, EPOLLERR not set
14. `testcase_epoll_hup_register_err` — register with EPOLLERR only, verify peer close still detected
15. `testcase_epoll_infinite` — timeout=-1 blocks until data arrives
16. `testcase_epoll_data_ptr` — data.ptr round-trips correctly
17. `testcase_epoll_create1_flag` — EPOLL_CLOEXEC flag
18. `testcase_epoll_merge` — EPOLLIN|EPOLLOUT on same fd merges to 1 event
19. `testcase_epoll_pri` — socketpair + MSG_OOB → verify EPOLLPRI reported
20. `testcase_epoll_concurrent` — pthread writer + epoll_wait(-1) blocking → concurrent wake-up

Platform-specific code is guarded with `#ifndef WIN32`.

## Coding Conventions

- **C Standard:** Not forced; compiler default (typically C11 or newer). Minimum C99 compatibility.
- **Naming:** uepoll internal functions prefixed with `uepoll_`; kepoll internal functions prefixed with `kepoll_` or `_kepoll_`; pepoll internal functions prefixed with `ppoll_`.
- **Spinlock abstraction:** `epoll_spinlock_init/lock/unlock` macros provide a uniform interface, with compile-time selection of the backing implementation.
- **Memory allocation:** All internal allocations go through the `epoll_realloc` function pointer (defaults to `realloc`), replaceable via `epoll_allocator()`.
- **Error handling:** Return `-1` and set `errno` (`EBADF`, `EINVAL`, `ENOENT`, `EEXIST`, `ENOMEM`, `EPERM`, `EFAULT`, `ENOSPC`).
- **Platform isolation:** `#ifndef WIN32` wraps entire `EPOLL_TEST_FUNCTION` calls (not inside macro arguments, since MSVC rejects preprocessor directives in macro parameters).
- **`HANDLE` type varies by platform:** Linux=`int`, Windows=`void*`, others=`intptr_t`. Casting `HANDLE` to `struct epoll_t*` in the code is intentional.

## Common Pitfalls

1. **`epoll_event` layout differs across backends:** uepoll's `epoll_event` has an internal `_nouse` field; wepoll does not. Binary sharing of event structs between backends is unsafe. On 64-bit platforms, `events` and `data` sit at the same offsets as Linux `<sys/epoll.h>`.
2. **fd upper bound:** The uepoll (select) backend is limited by `FD_SETSIZE` (default 1024). pepoll (poll) has **no limit** (bounded by `RLIMIT_NOFILE`). kepoll has a hardcoded kqueue event buffer of `EPOLL_MAX_EVENTS = 4096`.
3. **ET mode is non-portable:** Only kqueue (`EV_CLEAR`) and Linux native epoll support `EPOLLET`. pepoll and uepoll are always level-triggered.
4. **wepoll only supports SOCKET/Pipe:** Regular file descriptors cannot be used with epoll on Windows.
5. **`EPFD_POLL` defaults to ON:** CMake on BSD/Darwin compiles `pepoll.c` (poll) by default, not `kepoll.c` (kqueue). Pass `-DEPFD_POLL=OFF` to use kqueue.
6. **`epoll_create(0)` fails on Linux:** The Linux kernel rejects `size == 0` with `EINVAL` (since 2.6.8). Always use `epoll_create(1)` or larger for portable code.
7. **NULL events to `epoll_wait`:** On Linux kernel, `epoll_wait(efd, NULL, ...)` may return 0 instead of `EFAULT` if no events are pending (kernel only validates the pointer when it has data to write). Our non-Linux backends always check `events != NULL` upfront.
8. **Self-waking pipe fd drain:** The pipe read end (`pipes[0]`) is registered in the epoll instance itself. The drain loop uses a 1024-byte buffer and reads in a `while` loop — fully clearing the pipe under heavy concurrent modification.
9. **Concurrent `epoll_close` safety:** All non-Linux backends now detect concurrent close via a `closing` flag and interrupt blocking waits. However, production code should still ensure only one thread calls `epoll_close` per handle — the mechanism is a safety net, not a license to race.

## Modification Guide

- **Adding a new backend:** Create `new_backend.c`, add an `#elif` branch in `epoll.h`, add an `elseif` build rule in `CMakeLists.txt`.
- **Adding a new API:** Add the signature in all relevant headers (the Linux section of `epoll.h`, `wepoll.h`, `uepoll.h`) and implement in all backend `.c` files.
- **Modifying event constants:** Synchronize `enum EPOLL_EVENTS` and the corresponding `#define` macros in both `uepoll.h` and `wepoll.h`.
- **Testing:** Add a new `EPOLL_TEST_FUNCTION` test case in `main.c`. Guard platform-specific code with `#ifndef WIN32`.
