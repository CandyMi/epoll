message("======================================")
message("Project Name   : epoll")
message("Author  Name   : CandyMi")
message("Author  Email  : 869646063@qq.com")
message("Author  Github : github.com/CandyMi")
message("======================================")

cmake_minimum_required(VERSION 3.10)
project("epoll")

# ── AddressSanitizer ─────────────────────────
option(ENABLE_ASAN "Enable AddressSanitizer for debugging" OFF)
if(ENABLE_ASAN)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address -g -O1 -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  elseif(MSVC)
    message(STATUS "MSVC ASAN: set /fsanitize=address in CMAKE_C_FLAGS manually")
  endif()
endif()

message("-- Detecting OS: '${CMAKE_SYSTEM_NAME}'")

# ── Backend selection ────────────────────────
# ppoll.c (poll(2)) is the default POSIX fallback.  Unlike select:
#   • No FD_SETSIZE limit — supports up to RLIMIT_NOFILE.
#   • Milliseconds direct — no timeval conversion.
#   • Reports EPOLLHUP / EPOLLPRI natively.
#
# Options:
#   -DEPFD_SELECT=ON   → force uepoll.c (select) on non-Linux/BSD systems
#   -DEPFD_POLL=ON     → force ppoll.c on BSD/Darwin (override kqueue)
option(EPFD_SELECT "Use uepoll.c (select backend) instead of ppoll.c (poll)" OFF)
option(EPFD_POLL    "Use ppoll.c (poll backend) instead of kepoll.c (kqueue) on BSD/Darwin" ON)

if(CMAKE_SYSTEM_NAME MATCHES "Windows")
  set(epoll_src src/wepoll.c)
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
  set(epoll_src src/epoll.c)
elseif(CMAKE_SYSTEM_NAME MATCHES "BSD|Darwin")
  if(EPFD_POLL)
    set(epoll_src src/ppoll.c)
  else()
    set(epoll_src src/kepoll.c)
  endif()
else()
  if(EPFD_SELECT)
    set(epoll_src src/uepoll.c)
  else()
    set(epoll_src src/ppoll.c)
  endif()
endif()

# ── Object library — compile once ────────────
# When linked into a SHARED target CMake adds -fPIC automatically.
add_library(epoll_obj OBJECT ${epoll_src})
target_include_directories(epoll_obj PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/epoll>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/epoll>)

find_file(_have_atomic NAMES stdatomic.h)
if(_have_atomic)
  target_compile_definitions(epoll_obj PUBLIC EPOLL_USE_ATOMIC=1)
endif()

# ── Static library (all platforms) ───────────
add_library(epoll_static STATIC $<TARGET_OBJECTS:epoll_obj>)
target_link_libraries(epoll_static PUBLIC epoll_obj)
if(WIN32)
  set_target_properties(epoll_static PROPERTIES OUTPUT_NAME "epoll" PREFIX "")
else()
  set_target_properties(epoll_static PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
endif()

# ── Shared library (not on Windows; static-only avoids .lib collision) ──
if(NOT WIN32)
  add_library(epoll_dynamic SHARED $<TARGET_OBJECTS:epoll_obj>)
  target_link_libraries(epoll_dynamic PUBLIC epoll_obj)
endif()

# ── Platform link deps ───────────────────────
if(WIN32)
  target_link_libraries(epoll_static PUBLIC ws2_32)
endif()

# ── Test ─────────────────────────────────────
add_executable(main test/main.c)
target_link_libraries(main PRIVATE epoll_static)

# ── Backend identification for test suite ────
if(epoll_src STREQUAL "src/ppoll.c")
  target_compile_definitions(main PRIVATE EPOLL_BACKEND_POLL=1)
endif()

# ── Install ──────────────────────────────────
include(GNUInstallDirs)

set(epoll_targets epoll_static)
if(NOT WIN32)
  list(APPEND epoll_targets epoll_dynamic)
endif()

install(TARGETS ${epoll_targets}
        EXPORT epoll-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES include/epoll/epoll.h
              include/epoll/uepoll.h
              include/epoll/wepoll.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/epoll)
