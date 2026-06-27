<div align="center">

# epoll

[![CI](https://github.com/CandyMi/epoll/workflows/CI/badge.svg)](https://github.com/CandyMi/epoll/actions)
[![License](https://img.shields.io/badge/license-MIT-informational)](LICENSE)
[![C](https://img.shields.io/badge/C-C99%20%2F%20C11-blue?logo=c)](https://en.cppreference.com/w/c)
[![Platform](https://img.shields.io/badge/platform-Windows%20|%20Linux%20|%20macOS%20|%20BSD%20|%20POSIX-blue)](AGENTS.md)

  A project that uses `iocp` and `select` to simulate the ABI of `epoll`.

  So, Let's use `epoll` in every where. ：）

</div>

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

  * `EPOLLET` is supported on kqueue backend (macOS/BSD), not on select backend.
  * `wepoll` just supported `SOCKET`/`Pipe`.
  * `FD_SETSIZE` was limit the maximum number of fd.

# How to use it ?

  1. Copy all files to your project directory.
  2. Just include `"epoll.h"` file in your codes.
  3. Fine. Let's coding.

# License

  MIT