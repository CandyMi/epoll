# AGENTS.md — epoll Cross-Platform Compatibility Layer

## Project Overview

This is a **Linux epoll ABI compatibility layer** for cross-platform event-driven programming. Users simply `#include "epoll.h"` and write to the epoll API on Linux, Windows, macOS/FreeBSD, and any other POSIX system.

**Author:** CandyMi — [github.com/CandyMi](https://github.com/CandyMi)  
**License:** MIT

## Architecture

```mermaid
flowchart TD
    User["User Code"] -- import epoll.h --> epoll_h["epoll.h (Dispatch Layer)"]
    epoll_h -- "__linux__ / __ANDROID__\n(NO_NATIVE_EPOLL=0)" --> linux["epoll.c\nKernel epoll"]
    epoll_h -- "Linux + -DNO_NATIVE_EPOLL=1" --> uepoll_h
    epoll_h -- WIN32 --> win["wepoll.c\nIOCP + NT API"]
    epoll_h -- other --> uepoll_h["uepoll.h (types + API)"]
    uepoll_h -- "macOS / BSD" --> kqueue["kepoll.c\nkqueue / kevent"]
    uepoll_h -- "other POSIX" --> select["uepoll.c\nselect / fd_set"]
```

**Dispatch logic** in [`include/epoll/epoll.h`](include/epoll/epoll.h):
- `__linux__` / `__ANDROID__` + `NO_NATIVE_EPOLL=0` (default) → includes `<sys/epoll.h>` directly, defines thin shim functions
- `__linux__` + `-DNO_NATIVE_EPOLL=1` → includes `uepoll.h` (for CI/testing)
- `WIN32` → delegates to `wepoll.h`
- All others → delegates to `uepoll.h` (which is itself included by `kepoll.c` on BSD/Darwin at compile time)

**The actual backend selection is done by CMake at build time** ([`CMakeLists.txt:15-32`](CMakeLists.txt:15)): only one `.c` file is compiled per platform. All backends share the same 6-function API signature.

## File Inventory

```
epoll/
├── CMakeLists.txt              Build system (OS-conditional compilation + install rules)
├── README.md                   User documentation (platforms, features, usage)
├── LICENSE                     MIT License
├── AGENTS.md                   Project reference (this file)
├── include/
│   └── epoll/
│       ├── epoll.h   (36 L)    Dispatch header — #if/#elif/#else selects backend;
│       │                         respects -DNO_NATIVE_EPOLL=1 to force uepoll on Linux
│       ├── uepoll.h  (104 L)   Non-Linux API types/constants/signatures (HANDLE=intptr_t)
│       └── wepoll.h  (119 L)   Windows API definitions (HANDLE=void*, SOCKET=uintptr_t)
├── src/
│   ├── epoll.c     (12 L)      Linux native shim — no-op allocator + close(2) wrapper
│   ├── kepoll.c    (339 L)     macOS/BSD kqueue backend — struct epoll_t {int efd; lock;
│   │                         kqevents[] heap buffer}
│   ├── uepoll.c    (464 L)     POSIX select fallback — self-waking pipe + fd_set
│   └── wepoll.c    (2184 L)    Windows IOCP backend (vendored piscisaureus/wepoll)
├── test/
│   └── main.c      (507 L)     18 test cases via EPOLL_TEST_FUNCTION macro;
│                                 guarded for WIN32 / uses epoll_create(1)
└── docs/
    └── api.md                  Full API reference (functions, flags, ET/LT, examples, build)
```

## Public API (6 functions, uniform across all backends)

| Function | Signature Location | Semantics |
|---|---|---|
| `epoll_create(int size)` | [`uepoll.h:91`](uepoll.h:91) / [`wepoll.h:101`](wepoll.h:101) | Create epoll instance; `size` is ignored (matches Linux 2.6.8+ behavior) |
| `epoll_create1(int flags)` | [`uepoll.h:93`](uepoll.h:93) / [`wepoll.h:102`](wepoll.h:102) | Supports `EPOLL_CLOEXEC` |
| `epoll_ctl(HANDLE, int op, SOCKET, struct epoll_event*)` | [`uepoll.h:97`](uepoll.h:97) / [`wepoll.h:106`](wepoll.h:106) | ADD / MOD / DEL operations |
| `epoll_wait(HANDLE, struct epoll_event*, int maxevents, int timeout)` | [`uepoll.h:99`](uepoll.h:99) / [`wepoll.h:111`](wepoll.h:111) | Wait for events; `timeout=-1` blocks indefinitely |
| `epoll_close(HANDLE)` | [`uepoll.h:95`](uepoll.h:95) / [`wepoll.h:104`](wepoll.h:104) | Release all resources |
| `epoll_allocator(epoll_realloc_t)` | [`uepoll.h:89`](uepoll.h:89) / [`wepoll.h:100`](wepoll.h:100) | Replace internal `malloc`/`realloc`/`free` |

## Backend-Specific Design Decisions

### epoll.c — Linux Native Shim

- [`epoll.c:5-7`](epoll.c:5): `epoll_allocator` is a no-op (`(void)alloc`) — no internal allocation on Linux.
- [`epoll.c:10-12`](epoll.c:10): `epoll_close` directly calls `close(efd)`.

### kepoll.c — kqueue Backend

- **HANDLE is a pointer**: [`kepoll.c:50`](kepoll.c:50) `struct epoll_t` wraps a kqueue fd + spinlock + heap-allocated `kqevents[]` buffer. `epoll_create1` pre-allocates the kevent buffer (sized `EPOLL_MAX_EVENTS`) to avoid `alloca()` on every `epoll_wait`. On OOM, the kqueue fd is closed and the allocation is freed.
- **Event mapping** ([`kepoll.c:123-145`](kepoll.c:123)): `EPOLLIN|RDNORM|RDBAND|ERR|HUP → EVFILT_READ`, `EPOLLOUT|WRNORM|WRBAND → EVFILT_WRITE`. Non-requested filters are registered as `EV_DISABLE`. `EPOLLERR`/`EPOLLHUP` also enable `EVFILT_READ` so EOF/error can be detected.
- **EPOLLPRI support** ([`kepoll.c:160-172`](kepoll.c:160)): `EPOLLPRI → EVFILT_EXCEPT | NOTE_OOB` via a separate `kevent()` call (isolated from pipe-unsupported `EINVAL`). Guarded by `#if defined(EVFILT_EXCEPT) && defined(NOTE_OOB)` — only available on macOS; FreeBSD and other BSDs skip EPOLLPRI.
- **Event merging** ([`kepoll.c:262-337`](kepoll.c:262)): kqueue returns separate READ/WRITE events for the same fd. A `uintptr_t before_fd` cache merges adjacent same-fd events into a single `epoll_event`. ERROR/EOF/ONESHOT events are handled separately and counted in the return value.
- **ET mode** ([`kepoll.c:137`](kepoll.c:137)): `EPOLLET → EV_CLEAR` flag on kevent.
- **ONESHOT handling** ([`kepoll.c:259-275`](kepoll.c:259)): After returning from `epoll_wait`, `EV_ONESHOT` events are first written to the user `events[]` buffer, then `kepoll_del(ep, fd, false)` is called (DISABLE only, not DELETE).
- **NULL event validation** ([`kepoll.c:124-126`](kepoll.c:124)): `_kepoll_register` checks for NULL `event` and returns `EFAULT` before taking the spinlock.
- **Thread safety** ([`kepoll.c:17-42`](kepoll.c:17)): 3-level degrading spinlock (C11 `atomic_flag` → GCC `__sync_*` → `#error`). No pthread fallback.
- **Hardcoded maxevents**: [`kepoll.c:44`](kepoll.c:44) `#define EPOLL_MAX_EVENTS 4096`.

### uepoll.c — select Backend

- **Core data structure** ([`uepoll.c:76-81`](uepoll.c:76)): `struct epoll_t` contains `nfds` + `pipes[2]` (self-waking pipe) + `epoll_lock_t` + three `fd_set`s + `epoll_event udata[FD_SETSIZE]`.
- **fd = array index** ([`uepoll.c:80`](uepoll.c:80)): `udata[fd]` — this is why fd upper bound = `FD_SETSIZE` (default 1024 per [`uepoll.h:16-17`](uepoll.h:16)).
- **Self-waking mechanism** ([`uepoll.c:82-88`](uepoll.c:82)): `epoll_notice` macro writes `"x"` (1 byte) to `pipes[1]` after `epoll_ctl` modifies the fd sets, waking any `epoll_wait` blocked in `select()`. [`uepoll.c:83-88`](uepoll.c:83) `epoll_receive` drains the pipe with a 1024-byte buffer in a `while` loop (fully clears the pipe to prevent spurious wake-ups under heavy concurrent modification). The pipe read end (`pipes[0]`) is registered into the epoll instance itself during `epoll_create1` ([`uepoll.c:364`](uepoll.c:364)), and `epoll_wait` detects the wake via `FD_ISSET(ep->pipes[0], &rset)`.
- **Thread safety 3-level degrading** ([`uepoll.c:23-45`](uepoll.c:23)):
  - Level 0: `EPOLL_NO_THREADS` macro → no-ops
  - Level 1: C11 `<stdatomic.h>` → `atomic_flag` TAS spinlock
  - Level 2: GCC `__sync_*` builtins → spinlock
  - If none available → `#error` compilation failure (no pthread fallback)
- **ONESHOT handling** ([`uepoll.c:436-444`](uepoll.c:436)): After `epoll_wait` returns, the fd is removed from the `fd_set` (disabled, not deleted).
- **fd type validation** ([`uepoll.c:131-136`](uepoll.c:131)): `uepoll_can_poll` only allows sockets, FIFOs, character devices, and regular files.
- **EPOLLET unsupported**: The select model cannot simulate edge-triggered behavior.
- **Timeout recalculation** ([`uepoll.c:407-413`](uepoll.c:407)): After a self-wake pipe interruption, `CLOCK_MONOTONIC_COARSE` (or fallbacks at [`uepoll.c:99-115`](uepoll.c:99)) recalculates remaining timeout.

### wepoll.c — Windows IOCP Backend

- Third-party library ([piscisaureus/wepoll](https://github.com/piscisaureus/wepoll)); not directly maintained here.
- Only supports `SOCKET`/`Pipe` — regular files are unsupported.
- `HANDLE = void*`, `SOCKET = uintptr_t` ([`wepoll.h:56-57`](wepoll.h:56)).
- `epoll_event` has **no `_nouse` field** ([`wepoll.h:89-92`](wepoll.h:89)), unlike the uepoll variant.

## Supported Platforms & Compilers

| Platform | CMake Match | Preprocessor | Backend | Mechanism |
|---|---|---|---|---|
| **Linux** | `Linux` | `__linux__` | `epoll.c` | Kernel epoll |
| **Android** | `Android` | `__ANDROID__` | `epoll.c` | Kernel epoll |
| **Windows** | `Windows` | `WIN32` | `wepoll.c` | IOCP + NT API |
| **macOS** | `Darwin` | fallback | `kepoll.c` | kqueue/kevent |
| **FreeBSD / OpenBSD / NetBSD** | `BSD` | fallback | `kepoll.c` | kqueue/kevent |
| **Other POSIX** (Solaris, AIX, QNX, Haiku, etc.) | `else()` | fallback | `uepoll.c` | select |

**Compilers:** MSVC / GCC / Clang  
**C Standard:** Not forced; modern compilers default to C11/C17. Minimum requirement is C99 (`for`-loop declarations, `<stdbool.h>`, `//` comments). C11 atomics auto-detected via `__STDC_VERSION__ >= 201112L` ([`kepoll.c:22`](kepoll.c:22) / [`uepoll.c:31`](uepoll.c:31)), with GCC `__sync_*` fallback.  
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
- BSD/Darwin → `src/kepoll.c`
- Other → `src/uepoll.c`

**Build artifacts:** shared library (`libepoll.so`/`.dylib`/`.dll`) + static library (`libepoll.a`/`.lib`) + test executable (`build/main`).

**AddressSanitizer** (GCC/Clang, debug builds):
```bash
cmake -S . -B build_asan -DENABLE_ASAN=ON
cmake --build build_asan
./build_asan/main
```
On MSVC, use `/fsanitize=address` in `CMAKE_C_FLAGS` manually.

**Force select backend on Linux** (for testing):
```bash
cmake -S . -B build -DNO_NATIVE_EPOLL=1
cmake --build build
```

Install headers to `${CMAKE_INSTALL_INCLUDEDIR}/epoll/` and libraries to `${CMAKE_INSTALL_LIBDIR}`.

### Test

```bash
./build/main
```

### CI (GitHub Actions)

`.github/workflows/ci.yml` builds and tests all four backends × compilers on every push/PR:

| Job | OS | Backend | Compiler |
|---|---|---|---|
| `linux-gcc` | ubuntu-latest | `src/epoll.c` (kernel epoll) | GCC |
| `linux-clang` | ubuntu-latest | `src/epoll.c` (kernel epoll) | Clang |
| `linux-select` | ubuntu-latest | `src/uepoll.c` (select fallback) | GCC |
| `macos-clang` | macos-latest | `src/kepoll.c` (kqueue) | Clang |
| `windows-msvc` | windows-latest | `src/wepoll.c` (IOCP) | MSVC |
| `windows-clang` | windows-latest | `src/wepoll.c` (IOCP) | ClangCL |
| `windows-mingw` | windows-latest | `src/wepoll.c` (IOCP) | MinGW-w64 (GCC) |

18 active test cases registered in `main()` ([`test/main.c:486-505`](test/main.c:486)):
1. `testcase_epoll_timer` — epoll_wait timeout accuracy
2. `testcase_epoll_alloc` — rapid create/close (100k iterations)
3. `testcase_epoll_watch` — basic EPOLLIN watch on pipe
4. `testcase_epoll_repeat` — 100k repeated epoll_wait with persistent data
5. `testcase_epoll_oneshot` — EPOLLONESHOT disables after first trigger (also verifies events field has EPOLLIN)
6. `testcase_epoll_ctl_del` — delete then verify fd no longer watched
7. `testcase_epoll_ctl_mod` — modify data.ptr mid-stream
8. `testcase_epoll_out` — EPOLLOUT on writable pipe
9. `testcase_epoll_multi_fd` — two pipes both readable
10. `testcase_epoll_ctl_error` — invalid arguments to epoll_ctl/wait
11. `testcase_epoll_hup` — close peer, verify epoll_close works
12. `testcase_epoll_hup_flag` — pipe close → verify EPOLLHUP set, EPOLLERR not set
13. `testcase_epoll_hup_register_err` — register with EPOLLERR only, verify peer close still detected
14. `testcase_epoll_infinite` — timeout=-1 blocks until data arrives
15. `testcase_epoll_data_ptr` — data.ptr round-trips correctly
16. `testcase_epoll_create1_flag` — EPOLL_CLOEXEC flag
17. `testcase_epoll_merge` — EPOLLIN|EPOLLOUT on same fd merges to 1 event
18. `testcase_epoll_pri` — socketpair + MSG_OOB → verify EPOLLPRI reported

`testcase_epoll_et_mode` ([`test/main.c:74`](test/main.c:74)) is **commented out** because the select backend does not support EPOLLET. Platform-specific code is guarded with `#ifndef WIN32`.

## Coding Conventions

- **C Standard:** Not forced; compiler default (typically C11 or newer). Minimum C99 compatibility.
- **Naming:** uepoll internal functions prefixed with `uepoll_`; kepoll internal functions prefixed with `kepoll_` or `_kepoll_`.
- **Spinlock abstraction:** `epoll_spinlock_init/lock/unlock` macros provide a uniform interface, with compile-time selection of the backing implementation.
- **Memory allocation:** All internal allocations go through the `epoll_realloc` function pointer (defaults to `realloc`), replaceable via `epoll_allocator()` ([`uepoll.c:92`](uepoll.c:92)).
- **Error handling:** Return `-1` and set `errno` (`EBADF`, `EINVAL`, `ENOENT`, `EEXIST`, `ENOMEM`, `EPERM`, `EFAULT`, `ENOSPC`).
- **Platform isolation:** `#ifndef WIN32` wraps entire `EPOLL_TEST_FUNCTION` calls (not inside macro arguments, since MSVC rejects preprocessor directives in macro parameters).
- **`HANDLE` type varies by platform:** Linux=`int`, Windows=`void*`, others=`intptr_t`. Casting `HANDLE` to `struct epoll_t*` in the code is intentional.

## Common Pitfalls

1. **`epoll_event` layout differs across backends:** uepoll's `epoll_event` has an internal `_nouse` field ([`uepoll.h:79`](uepoll.h:79)); wepoll does not ([`wepoll.h:89-92`](wepoll.h:89)). Binary sharing of event structs between backends is unsafe. On 64-bit platforms, `events` and `data` sit at the same offsets as Linux `<sys/epoll.h>`.
2. **fd upper bound:** The uepoll (select) backend is limited by `FD_SETSIZE` (default 1024, [`uepoll.h:16-17`](uepoll.h:16)). kepoll has a hardcoded `EPOLL_MAX_EVENTS=1024` ([`kepoll.c:44`](kepoll.c:44)).
3. **ET mode is non-portable:** The select backend does not support `EPOLLET`. The kqueue backend supports it via `EV_CLEAR`.
4. **wepoll only supports SOCKET/Pipe:** Regular file descriptors cannot be used with epoll on Windows.
5. **kepoll event merging requires sufficient maxevents:** `epoll_wait`'s `maxevents` must be ≥ the actual number of kqueue events returned, otherwise the merge logic may truncate (see [`test/main.c:404`](test/main.c:404) test comment).
6. **`epoll_create(0)` fails on Linux:** The Linux kernel rejects `size == 0` with `EINVAL` (since 2.6.8). Always use `epoll_create(1)` or larger for portable code.
7. **`epoll_close` behavior differs:** kepoll returns `-1` when `efd <= 0` ([`kepoll.c:72-73`](kepoll.c:72)); the Linux shim calls `close(efd)` directly ([`epoll.c:9-11`](epoll.c:9)).
8. **NULL events to `epoll_wait`:** On Linux kernel, `epoll_wait(efd, NULL, ...)` may return 0 instead of `EFAULT` if no events are pending (kernel only validates the pointer when it has data to write). Our non-Linux backends always check `events != NULL` upfront.
9. **Self-waking pipe fd drain:** The pipe read end (`pipes[0]`) is registered in the epoll instance itself ([`uepoll.c:364`](uepoll.c:364)). The drain loop uses a 1024-byte buffer and reads in a `while` loop — fully clearing the pipe under heavy concurrent modification.

## Modification Guide

- **Adding a new backend:** Create `new_backend.c`, add an `#elif` branch in `epoll.h`, add an `elseif` build rule in `CMakeLists.txt`.
- **Adding a new API:** Add the signature in all relevant headers (the Linux section of `epoll.h`, `wepoll.h`, `uepoll.h`) and implement in all backend `.c` files.
- **Modifying event constants:** Synchronize `enum EPOLL_EVENTS` and the corresponding `#define` macros in both `uepoll.h` and `wepoll.h`.
- **Testing:** Add a new `EPOLL_TEST_FUNCTION` test case in `main.c`. Guard platform-specific code with `#ifndef WIN32`.
