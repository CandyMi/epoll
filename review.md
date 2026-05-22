# 项目设计分析：跨平台 `epoll` 兼容层

该项目提供一个**Linux epoll ABI 的跨平台模拟层**，使得头文件 `epoll.h` 可以"一处包含，到处编译"。通过预处理器分派到四个后端之一，每个后端在各自平台上导出完全相同的函数签名。

---

## 一、总架构图

```
                    ┌─────────────────────┐
                    │      epoll.h        │  ← 用户的唯一条目头文件
                    │  (分派层 Dispatcher)│
                    └────────┬────────────┘
                             │ 预处理器分支
            ┌────────────────┼────────────────┬────────────────┐
            ▼                ▼                ▼                ▼
     Linux/Android        Windows        macOS/FreeBSD      其他 POSIX
    ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
    │   epoll.c    │ │  wepoll.c    │ │   kepoll.c   │ │   uepoll.c   │
    │  (原生垫片)   │ │  (IOCP模拟)   │ │ (kqueue模拟)  │ │ (select模拟) │
    │  ~10行       │ │   ~2185行     │ │  ~230行      │ │  ~420行      │
    └──────────────┘ └──────────────┘ └──────────────┘ └──────────────┘
```

**分派逻辑** (`epoll.h`):

| 平台宏 | 行为 |
|---|---|
| `__linux__` 或 `__ANDROID__` | `#define NO_NATIVE_EPOLL 0`，包含系统 `<sys/epoll.h>`，只提供 `epoll_allocator` / `epoll_close` 两个垫片函数（其余来自内核） |
| `WIN32` | 包含 `wepoll.h`，完全委托给 wepoll 的 IOCP 模拟 |
| 其他 | 包含 `uepoll.h`，走 select/kqueue 模拟 |

---

## 二、四个后端的对比

| 特性 | **epoll.c** (Linux) | **wepoll.c** (Windows) | **kepoll.c** (macOS/BSD) | **uepoll.c** (POSIX fallback) |
|---|---|---|---|---|
| **底层机制** | 内核 `epoll` | IOCP + NT 内核 API | `kqueue` / `kevent` | `select` |
| **作者** | CandyMi (垫片) | Bert Belder (libuv) | CandyMi | CandyMi |
| **NO_NATIVE_EPOLL** | 0 | 1 | 1 | 1 |
| **EPOLLET 支持** | 原生支持 | 部分 | 支持(`EV_CLEAR`) | **不支持** |
| **EPOLLONESHOT** | 原生支持 | 支持 | 支持(`EV_ONESHOT`) | 支持 |
| **最大 fd 数** | 无硬限制 | 无硬限制 | 1024(硬编码) | `FD_SETSIZE`(通常1024) |
| **HANDLE 类型** | `int` (fd) | `void*` | `intptr_t` (包装指针) | `intptr_t` (包装指针) |
| **线程安全** | 内核自带 | IOCP 原生 | 自旋锁(C11/GCC) | 自旋锁4级降级 |
| **代码量** | ~10 行 | ~2185 行 | ~230 行 | ~420 行 |

---

## 三、后端详细设计

### 1. `epoll.c` — Linux 原生垫片 (`epoll.c` + `epoll.h`)

- 真正的"空壳"——只实现 `epoll_allocator()`（空操作）和 `epoll_close()`（转为 `close(2)`）。
- `epoll_create`、`epoll_ctl`、`epoll_wait` 全部来自内核头文件 `<sys/epoll.h>` 和内核模块。
- `HANDLE` / `SOCKET` 都 `typedef int`，因为 Linux 上一切皆 fd。

### 2. `kepoll.c` — kqueue 后端 (`kepoll.c`)

- 将 epoll 事件映射到 kqueue filter：

```
EPOLLIN | EPOLLRDNORM | EPOLLRDBAND  →  EVFILT_READ
EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND →  EVFILT_WRITE
```

- **关键设计：事件合并**。kqueue 可能对同一个 fd 返回分开的 `EVFILT_READ` 和 `EVFILT_WRITE` 事件，但 epoll 期望一个 `epoll_event` 同时包含 `EPOLLIN|EPOLLOUT`。`epoll_wait` 中通过 `before_fd` 缓存实现相邻同 fd 事件的合并：

```c
bool eqfd = before_fd == kqevents[i].ident;
if (!eqfd) {
    before_fd = kqevents[i].ident;
    ev = &events[i];
    ev->events = EPOLLEMPTY;
    ev->data.ptr = kqevents[i].udata;
    nkqevents++; pos = i;
} else {
    ev = &events[pos];  // 合并到已有的 event 条目
}
```

- **ET 模式**：通过 `EV_CLEAR` 标志模拟（`EV_ONESHOT | EV_CLEAR` 不走默认 `EV_DISABLE` 逻辑）。
- **线程安全**：三档编译时选择（C11 `atomic_flag` → GCC 内置原子操作 → 编译错误 `#error`），避免在 macOS 上依赖 pthread。
- 注册/修改都通过同一个 `_kepoll_register()` 函数，区别仅在于首次是否加 `EV_ADD` 标志。

### 3. `uepoll.c` — select 后端 (`uepoll.c`)

- **核心数据结构**：

```c
struct epoll_t {
    bool edited;
    int nfds;
    int pipes[2];         // 自唤醒管道
    epoll_lock_t lock;    // 自旋锁
    fd_set rset, wset, eset;
    epoll_event udata[EPOLL_MAX_EVENTS];  // FD_SETSIZE 长度的数组
};
```

- 每个 fd 就是 `udata[fd]` 的数组下标——这是 select 后端的 fd 上限硬绑定的根本原因。
- **自唤醒机制**：当其他线程调用 `epoll_ctl` 修改监听集合时，通过 `write(pipe[1], "", 0)` 唤醒阻塞在 `select()` 中的 `epoll_wait`，使其重新计算超时后再次进入 `select()`。
- **线程安全**：4 级降级设计：

| 级别 | 条件 | 锁实现 |
|---|---|---|
| 0 (最快) | `EPOLL_NO_THREADS` 定义 | 空操作（无锁） |
| 1 | C11 `<stdatomic.h>` | `atomic_flag` TAS 自旋 |
| 2 | GCC `__sync_*` 内置 | GCC 原子 TAS 自旋 |
| 3 (最兼容) | 以上皆无 | `dlopen` 动态加载 `pthread_mutex` |

- **事件循环流程**：
  1. 加锁复制三组 `fd_set` 到栈上
  2. 解锁，调用 `select(nfds, &rset, &wset, &eset, timeout)`
  3. 如果 pipe[0] 就绪 → 说明被其他线程唤醒，重新计算超时后重入
  4. 遍历 `0..nfds`，将就绪 fd 填入 `events[]`
  5. 如果含 `EPOLLONESHOT` 则从集合中清除该 fd
- 支持 `pipe`、`socket`、`FIFO`、字符设备、甚至普通文件（通过 `fstat` + `S_ISSOCK/S_ISFIFO/S_ISCHR/S_ISREG` 校验）。

### 4. `wepoll.c` — Windows IOCP 后端

- 第三方库 [piscisaureus/wepoll](https://github.com/piscisaureus/wepoll)，2185 行。
- 底层使用 Windows IOCP + NT 内核 API（`NtCreateIoCompletion`、`NtSetInformationFile` 等）。
- 本项目中作为纯依赖引用，不展开 re-architect。

---

## 四、统一 API 契约

所有后端导出的公共 API（6 个函数）：

| 函数 | 语义 |
|---|---|
| `epoll_create(int size)` | 创建 epoll 实例，size 被忽略（与 Linux 2.6.8+ 行为一致） |
| `epoll_create1(int flags)` | 同上，支持 `EPOLL_CLOEXEC` |
| `epoll_ctl(HANDLE, op, SOCKET, event)` | 控制事件（ADD/MOD/DEL） |
| `epoll_wait(HANDLE, events, max, timeout)` | 等待事件，timeout=-1 无限等待 |
| `epoll_close(HANDLE)` | 关闭 epoll 实例 |
| `epoll_allocator(epoll_realloc_t)` | 替换内部内存分配器 |

**类型适配**：

| 类型 | Linux | Windows | macOS/BSD/其他 |
|---|---|---|---|
| `HANDLE` | `int` | `void*` | `intptr_t` |
| `SOCKET` | `int` | `uintptr_t` | `int` |
| `epoll_data` 联合体 | 含 `sock` / `hnd` 字段 | 含 `sock` / `hnd` 字段 | 含 `sock` / `hnd` 字段 |

`epoll_event` 结构在各后端有细微差别：
- **wepoll**: `{ uint32_t events; epoll_data_t data; }` — 标准布局
- **uepoll**: `{ int32_t _nouse; uint32_t events; epoll_data_t data; }` — 多了内部使用的 `_nouse` 字段
- **系统 epoll**: 标准 Linux 布局

---

## 五、构建系统 (`CMakeLists.txt`)

OS 条件编译选择正确的后端源文件：

```cmake
if(WIN32)         → wepoll.c
elseif(Linux/Android) → epoll.c
elseif(BSD/Darwin)    → kepoll.c
else()                → uepoll.c  # 通用 fallback
```

每个平台都生成 `epoll` 名称的动态库和静态库，并安装三个头文件到 `<prefix>/include/epoll/`。

---

## 六、测试套件 (`main.c`)

使用宏 `EPOLL_TEST_FUNCTION` 定义 15 个测试用例：

| 测试 | 验证点 |
|---|---|
| `testcase_epoll_timer` | timeout 准确性 |
| `testcase_epoll_alloc` | 重复 create/close 无泄漏 |
| `testcase_epoll_watch` | 基本 EPOLLIN 读写路径 |
| `testcase_epoll_repeat` | 100k 次重复 wait 稳定性 |
| `testcase_epoll_oneshot` | EPOLLONESHOT 一次触发后消失 |
| `testcase_epoll_ctl_del` | 删除后不再返回事件 |
| `testcase_epoll_ctl_mod` | 修改 data.ptr 生效 |
| `testcase_epoll_out` | EPOLLOUT 写就绪 |
| `testcase_epoll_multi_fd` | 多 fd 同时就绪 |
| `testcase_epoll_ctl_error` | 错误参数返回 -1 并设 errno |
| `testcase_epoll_hup` | 对端关闭后的资源释放 |
| `testcase_epoll_infinite` | timeout=-1 阻塞直到数据到达 |
| `testcase_epoll_data_ptr` | data.ptr 原样返回 |
| `testcase_epoll_create1_flag` | EPOLL_CLOEXEC 标志 |
| `testcase_epoll_merge` | 同 fd READ+WRITE 合并为一条事件 |

ET 模式测试被注释掉（`// testcase_epoll_et_mode()`），因为 select 后端不支持 ET。

---

## 七、设计亮点与取舍

**亮点**:
- **单一头文件分派** — 用户只需 `#include "epoll.h"`，零配置跨平台。
- **自旋锁多级降级** — uepoll 的锁选择从最快到最兼容，在轻量级自旋和广泛兼容之间做了很好的平衡。
- **自唤醒 pipe** — 解决了 select 在等待期间无法响应集合修改的问题。
- **事件合并** — kepoll 正确处理了 kqueue 双 filter 与 epoll 单 event 模型的差异。
- **可插拔分配器** — `epoll_allocator` 允许嵌入方自定义内存管理。

**限制** (文档明确标注):
- `uepoll` 不支持 `EPOLLET`（边沿触发）。
- `uepoll` 受 `FD_SETSIZE` 限制（典型值 1024）。
- `wepoll` 只支持 `SOCKET` / `Pipe`，不支持普通文件 fd。
- `uepoll` maxevents 为硬编码 1024。

---

## 八、文件依赖关系

```
epoll.h  ──┬──→ 系统 <sys/epoll.h>  (Linux)
           ├──→ wepoll.h ──→ wepoll.c  (Windows)
           └──→ uepoll.h ──┬──→ uepoll.c  (select fallback)
                           └──→ kepoll.c  (kqueue, #include "uepoll.h" 但只用其类型定义)

main.c ──→ epoll.h  (测试入口)
```
