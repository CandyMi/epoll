# epoll

  A project that uses `iocp` and `select` to simulate the ABI of `epoll`.

  So, Let's use `epoll` in every where. ：）

# Support Platform

  * Windows (wepoll)
  * Linux
  * MacOS
  * FreeBSD
  * Other Posix

# Support C Compiler

  * MSVC
  * GCC
  * CLANG

# Feature

  * Thread-Safe.
  * ABI Compatible.
  * Multi-Platform Supported.
  * `block` and `nonblock` Supported.
  * `fd` can `Insert` and `Remove` in anytime.

# Limit

  * `EPOLLET` is never supported.
  * `wepoll` just supported `SOCKET`/`Pipe`.
  * `FD_SETSIZE` was limit the maximum number of fd.

# How to use it ?

  1. Copy all files to your project directory.
  2. Just include `"epoll.h"` file in your codes.
  3. Fine. Let's coding.

# License

  MIT