# AGENTS.md — epoll 跨平台兼容层

## 项目定位

这是一个**Linux epoll ABI 的跨平台模拟层**。用户只需 `#include "epoll.h"`，即可在 Linux、Windows、macOS/FreeBSD 及其他 POSIX 系统上用同一个 API 编写事件驱动代码。

## 架构总览

```
                          ┌─────────────────────┐
                          │        epoll.h      │  ← 用户的唯一条目头文件
                          │  (分派层 Dispatcher) │
                          └──────────┬──────────┘
                                     │ 预处理器分支
            ┌────────────────┬───────┼────────┬────────────────┐
            ▼                ▼                ▼                ▼
     Linux/Android        Windows        macOS/FreeBSD      其他 POSIX
    ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
    │   epoll.c    │ │  wepoll.c    │ │   kepoll.c   │ │   uepoll.c   │
    │    epoll     │ │    IOCP      │ │    kqueue    │ │    select    │
    └──────────────┘ └──────────────┘ └──────────────┘ └──────────────┘
```

分派逻辑在 [`epoll.h:4-24`](epoll.h:4)：
- `__linux__` / `__ANDROID__` → `NO_NATIVE_EPOLL=0`，直接用内核 epoll
- `WIN32` → 委托给 `wepoll.h`
- 其他 → 委托给 `uepoll.h`（uepoll.h 内部按平台再分发到 uepoll.c 或 kepoll.c）

**实际后端选择由 CMakeLists.txt 的 OS 条件编译决定**（[`CMakeLists.txt:15-32`](CMakeLists.txt:15)）：构建时只编译一个 `.c` 文件，但所有后端共享同一套 API 签名。

## 文件清单

| 文件 | 角色 | 关键细节 |
|---|---|---|
| `epoll.h` | 分派层头文件 | 仅 ~24 行，纯 `#if/#elif/#else` |
| `uepoll.h` | 非 Linux 平台的 API 定义 | 定义 `HANDLE=intptr_t`、`SOCKET=int`、epoll 事件常量、6 个 API 签名；所有非 Linux 后端包含此文件 |
| `epoll.c` | Linux 垫片 | 只实现 `epoll_allocator()`（空操作）和 `epoll_close()`（→ `close(2)`），其余直接走内核 |
| `kepoll.c` | macOS/BSD kqueue 后端 | ~230 行，通过 `#include "uepoll.h"` 获取类型定义；kqueue fd 包装在 `struct epoll_t { int efd; epoll_lock_t lock; }` |
| `uepoll.c` | POSIX select fallback | ~420 行，自唤醒 pipe + fd_set 轮询；线程安全 3 级降级锁（无 pthread fallback） |
| `wepoll.c` | Windows IOCP 后端 | ~2185 行，第三方库 [piscisaureus/wepoll](https://github.com/piscisaureus/wepoll)，不展开 |
| `wepoll.h` | Windows API 定义 | 独立定义所有类型/常量/签名，`HANDLE=void*`，`SOCKET=uintptr_t`，无 `_nouse` 字段 |
| `main.c` | 测试套件 | 15 个 `EPOLL_TEST_FUNCTION` 宏定义的测试用例 |
| `CMakeLists.txt` | 构建系统 | OS 条件编译 + 动态库/静态库 + 安装规则 |
| `README.md` | 用户文档 | 平台/编译器支持、特性、限制、用法 |
| `et_and_lt.md` | ET/LT 模式说明 | 边沿触发/水平触发补充文档 |
| `LICENSE` | MIT 许可证 | — |

## 公共 API（6 个函数，所有后端统一）

| 函数 | 签名位置 | 语义 |
|---|---|---|
| `epoll_create(int size)` | [`uepoll.h:70`](uepoll.h:70) | 创建 epoll 实例，size 被忽略 |
| `epoll_create1(int flags)` | [`uepoll.h:72`](uepoll.h:72) | 支持 `EPOLL_CLOEXEC` |
| `epoll_ctl(HANDLE, int op, SOCKET, struct epoll_event*)` | [`uepoll.h:76`](uepoll.h:76) | ADD/MOD/DEL |
| `epoll_wait(HANDLE, struct epoll_event*, int maxevents, int timeout)` | [`uepoll.h:78`](uepoll.h:78) | timeout=-1 无限等待 |
| `epoll_close(HANDLE)` | [`uepoll.h:74`](uepoll.h:74) | 释放资源 |
| `epoll_allocator(epoll_realloc_t)` | [`uepoll.h:68`](uepoll.h:68) | 替换内部 `malloc`/`realloc`/`free` |

## 各后端关键设计决策

### epoll.c — Linux 原生垫片

- [`epoll.c:5-7`](epoll.c:5)：`epoll_allocator` 是空操作（`(void)alloc`），因为 Linux 上无内部分配。
- [`epoll.c:9-12`](epoll.c:9)：`epoll_close` 直接调用 `close(2)`。

### kepoll.c — kqueue 后端

- **HANDLE 是指针**：[`kepoll.c:54`](kepoll.c:54) `struct epoll_t` 包装 kqueue fd + 自旋锁，`epoll_create1` 在 [`kepoll.c:82-97`](kepoll.c:82) 分配此结构体（含 NULL 检查 + OOM 时关闭 kqueue fd）。
- **事件映射**：[`kepoll.c:96-121`](kepoll.c:96)：`EPOLLIN|RDNORM|RDBAND → EVFILT_READ`，`EPOLLOUT|WRNORM|WRBAND → EVFILT_WRITE`。
- **事件合并**：[`kepoll.c:162-197`](kepoll.c:162)：kqueue 为同一 fd 返回分开的 READ/WRITE 事件，通过 `uintptr_t before_fd` 缓存将相邻同 fd 事件合并为一条 `epoll_event`。ERROR/EOF 事件单独处理并计入返回值。
- **ET 模式**：[`kepoll.c:104-105`](kepoll.c:104)：`EPOLLET → EV_CLEAR` 标志。
- **ONESHOT 处理**：[`kepoll.c:172-174`](kepoll.c:172)：`epoll_wait` 返回后对 EV_ONESHOT 事件调用 `kepoll_del(ep, fd, false)`（仅 DISABLE，不 DELETE）。
- **线程安全**：[`kepoll.c:14-34`](kepoll.c:14)：3 级降级（C11 atomic_flag → GCC `__sync_*` → `#error`），无 pthread fallback。
- **maxevents 硬编码 1024**：[`kepoll.c:38`](kepoll.c:38) `#define EPOLL_MAX_EVENTS 1024`。

### uepoll.c — select 后端

- **核心数据结构**：[`uepoll.c:82-91`](uepoll.c:82) `struct epoll_t`：`nfds` + `pipes[2]`（自唤醒）+ `epoll_lock_t` + 三组 `fd_set` + `epoll_event udata[FD_SETSIZE]`。
- **fd 即数组下标**：[`uepoll.c:90`](uepoll.c:90) `udata[fd]` —— 这是 fd 上限 = `FD_SETSIZE`（典型 1024）的根本原因。
- **自唤醒机制**：[`uepoll.c:80`](uepoll.c:80) `epoll_notice` 宏：`epoll_ctl` 修改集合后 `write(pipes[1], "x", 1)` 向管道写入 1 字节唤醒阻塞在 `select()` 中的 `epoll_wait`；[`uepoll.c:93-99`](uepoll.c:93) `epoll_receive` 排空 pipe。`epoll_create1` 将 pipe 读端（`pipes[0]`）注册到 epoll 实例中（[`uepoll.c:405`](uepoll.c:405)），`epoll_wait` 通过 `FD_ISSET(ep->pipes[0], &rset)` 检测唤醒。
- **线程安全 3 级降级**：[`uepoll.c:23-43`](uepoll.c:23)：
  - Level 0: `EPOLL_NO_THREADS` 宏 → 空操作
  - Level 1: C11 `<stdatomic.h>` → `atomic_flag` TAS 自旋
  - Level 2: GCC `__sync_*` 内置原子 → 自旋
  - 如果以上皆不可用 → `#error` 编译失败（无 pthread fallback）
- **ONESHOT 处理**：[`uepoll.c:249-257`](uepoll.c:249) `epoll_wait` 返回后从 `fd_set` 清除该 fd。
- **fd 类型校验**：[`uepoll.c:121-126`](uepoll.c:121) `uepoll_can_poll`：仅允许 socket、FIFO、字符设备、普通文件。
- **不支持 EPOLLET**：select 模型无法模拟边沿触发。
- **超时重算**：[`uepoll.c:227-231`](uepoll.c:227)：被自唤醒 pipe 打断后，用 `CLOCK_MONOTONIC_COARSE` 重新计算剩余超时（[`uepoll.c:103-115`](uepoll.c:103) 时间源选择）。

### wepoll.c — Windows IOCP 后端

- 第三方库，不直接维护。只支持 `SOCKET`/`Pipe`，不支持普通文件。
- `HANDLE = void*`，`SOCKET = uintptr_t`（[`wepoll.h:69-70`](wepoll.h:69)）。
- `epoll_event` 结构无 `_nouse` 字段（[`wepoll.h:78-81`](wepoll.h:78)），与 uepoll 不同。

## 支持平台与编译器

| 平台 | CMake 匹配 | 预处理器 | 后端 | 机制 |
|---|---|---|---|---|
| **Linux** | `Linux` | `__linux__` | `epoll.c` | 内核 epoll |
| **Android** | `Android` | `__ANDROID__` | `epoll.c` | 内核 epoll |
| **Windows** | `Windows` | `WIN32` | `wepoll.c` | IOCP + NT API |
| **macOS** | `Darwin` | fallback | `kepoll.c` | kqueue/kevent |
| **FreeBSD** / **OpenBSD** / **NetBSD** | `BSD` | fallback | `kepoll.c` | kqueue/kevent |
| **其他 POSIX** (Solaris, AIX, QNX, Haiku 等) | `else()` | fallback | `uepoll.c` | select |

- **编译器**：MSVC / GCC / Clang
- **C 标准**：`CMAKE_C_STANDARD 99`（[`CMakeLists.txt:12`](CMakeLists.txt:12)）。实际最低基线为 **C99 + GNU `__sync_*` 扩展**；若 `<stdatomic.h>` 可用则自动启用 C11 原子操作（[`kepoll.c:21`](kepoll.c:21) / [`uepoll.c:29`](uepoll.c:29) 检查 `__STDC_VERSION__ >= 201112L`）。
- **架构**：32/64-bit 均支持。`HANDLE` 类型随指针宽度自适应（`intptr_t` / `void*`），无内联汇编或边端序假设。

## 构建与测试

### 构建

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

CMake 按 `CMAKE_SYSTEM_NAME` 选择后端（[`CMakeLists.txt:15-32`](CMakeLists.txt:15)）：
- Windows → `wepoll.c`
- Linux/Android → `epoll.c`
- BSD/Darwin → `kepoll.c`
- 其他 → `uepoll.c`

产物：动态库 `libepoll.so`/`.dylib`/`.dll` + 静态库 `libepoll.a`/`.lib` + 可执行测试 `main`。

### 测试

```bash
./build/main
```

15 个测试用例（[`main.c:16-253`](main.c:16)），`EPOLL_TEST_FUNCTION` 宏自动编号并打印结果。

ET 模式测试（`testcase_epoll_et_mode`）被注释掉（[`main.c:250`](main.c:250)），因为 select 后端不支持。

## 编码约定

- **C99 标准**：[`CMakeLists.txt:10`](CMakeLists.txt:10) `set(CMAKE_C_STANDARD 99)`。
- **命名前缀**：uepoll 内部函数以 `uepoll_` 为前缀；kepoll 内部函数以 `kepoll_` 或 `_kepoll_` 为前缀。
- **锁宏抽象**：`epoll_spinlock_init/lock/unlock` 宏统一接口，底层实现由编译时条件选择。
- **内存分配**：所有内部分配走 `epoll_realloc` 函数指针（默认 `realloc`），可通过 `epoll_allocator()` 替换（[`uepoll.c:100`](uepoll.c:100)）。
- **错误处理**：返回 `-1` 并设 `errno`（`EBADF`、`EINVAL`、`ENOENT`、`EEXIST`、`ENOMEM`、`EPERM`、`EFAULT`、`ENOSPC`）。
- **平台隔离**：`#ifndef WIN32` 守卫平台特有代码（如 `main.c` 中 pipe/socketpair 测试）。
- **`HANDLE` 类型因平台而异**：Linux=`int`，Windows=`void*`，其他=`intptr_t`。代码中对 `HANDLE` 做强转为 `struct epoll_t*` 是刻意的。

## 常见陷阱

1. **`epoll_event` 布局不同**：uepoll 的 `epoll_event` 有 `_nouse` 内部字段（[`uepoll.h:58`](uepoll.h:58)），wepoll 没有。跨后端共享二进制数据不安全。
2. **fd 上限**：uepoll（select）后端受 `FD_SETSIZE` 限制（默认 1024，[`uepoll.h:12`](uepoll.h:12)）。kepoll 有 `EPOLL_MAX_EVENTS=1024` 的硬编码限制（[`kepoll.c:38`](kepoll.c:38)）。
3. **ET 模式不可移植**：select 后端不支持 `EPOLLET`。README 明确标注此限制。
4. **wepoll 只支持 SOCKET/Pipe**：不能对普通文件描述符使用 epoll。
5. **kepoll 事件合并**：`epoll_wait` 的 `maxevents` 必须 ≥ kqueue 实际返回的事件数，否则合并逻辑可能截断（见 [`main.c:227`](main.c:227) 测试注释）。
6. **`epoll_create` 的 size 参数被忽略**：与 Linux 2.6.8+ 行为一致，但老代码可能依赖。
7. **kepoll 的 `epoll_close`** 在 `efd <= 0` 时返回 -1（[`kepoll.c:72-73`](kepoll.c:72)），而 Linux 垫片直接调用 `close(efd)`（[`epoll.c:11`](epoll.c:11)），行为略有不同。
8. **自唤醒 pipe fd 占用**：pipe 读端 `pipes[0]` 被注册进 epoll 实例本身（[`uepoll.c:405`](uepoll.c:405)），因此多线程下 `epoll_ctl` 修改 fd 集合后会通过管道唤醒 `epoll_wait` 使其重新计算超时后重入 `select()`。注意 `epoll_notice` 写 1 字节到 `pipes[1]`，需确保管道缓冲区不会被写满。

## 修改指南

- **添加新后端**：创建 `new_backend.c`，在 `epoll.h` 添加 `#elif` 分支，在 `CMakeLists.txt` 添加 `elseif` 构建规则。
- **添加新 API**：在所有四个后端的头文件（`epoll.h` Linux 段、`wepoll.h`、`uepoll.h`）中同步添加签名，在所有四个 `.c` 文件中实现。
- **修改事件常量**：需同步 `uepoll.h` 和 `wepoll.h` 中的 `enum EPOLL_EVENTS` 及对应的 `#define` 宏。
- **测试**：新功能必须在 `main.c` 添加 `EPOLL_TEST_FUNCTION` 测试用例，并用 `#ifndef WIN32` 等守卫平台特定代码。
