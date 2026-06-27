message("======================================")
message("Project Name   : epoll")
message("Author  Name   : CandyMi")
message("Author  Email  : 869646063@qq.com")
message("Author  Github : github.com/CandyMi")
message("======================================")

cmake_minimum_required(VERSION 3.10)

project("epoll")

# ── AddressSanitizer (GCC/Clang only; MSVC uses /fsanitize=address) ──
option(ENABLE_ASAN "Enable AddressSanitizer for debugging" OFF)
if(ENABLE_ASAN)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address -g -O1 -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
    message(STATUS "AddressSanitizer enabled")
  elseif(MSVC)
    message(STATUS "MSVC: use /fsanitize=address in CMAKE_C_FLAGS manually")
  endif()
endif()

# C standard not forced — modern compilers default to C11/C17.
# Minimum C99 (for-loop decl, stdbool.h, // comments).
message("-- Detecting OS: '${CMAKE_SYSTEM_NAME}'")

# ── Include path ──────────────────────────────
# Public so consumers (and test) get the header search path
add_library(epoll INTERFACE)
target_include_directories(epoll INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/epoll>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/epoll>)

# ── Backend selection ──────────────────────────
add_executable(main test/main.c)
if(CMAKE_SYSTEM_NAME MATCHES "Windows")
  file(GLOB epoll_src src/wepoll.c)
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
  file(GLOB epoll_src src/epoll.c)
elseif(CMAKE_SYSTEM_NAME MATCHES "BSD|Darwin")
  file(GLOB epoll_src src/kepoll.c)
else()
  file(GLOB epoll_src src/uepoll.c)
endif()

if(NOT WIN32)
  add_library(epoll_dynamic SHARED ${epoll_src})
  set_target_properties(epoll_dynamic PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
  target_link_libraries(epoll_dynamic PRIVATE epoll)
endif()

add_library(epoll_static STATIC ${epoll_src})

if(WIN32)
  set_target_properties(epoll_static PROPERTIES OUTPUT_NAME "epoll" PREFIX "")
  target_link_libraries(epoll_static PUBLIC ws2_32)
else()
  set_target_properties(epoll_static PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
endif()

target_link_libraries(epoll_static PUBLIC epoll)
target_link_libraries(main PRIVATE epoll_static)

find_file(EPOLL_ATOMIC NAMES stdatomic.h)
if (EPOLL_ATOMIC)
  if(NOT WIN32)
    target_compile_definitions(epoll_dynamic PRIVATE EPOLL_USE_ATOMIC=1)
  endif()
  target_compile_definitions(epoll_static PRIVATE EPOLL_USE_ATOMIC=1)
endif()

# ── Install rules ─────────────────────────────
include(GNUInstallDirs)

if(NOT WIN32)
  install(TARGETS epoll_static epoll_dynamic epoll
          EXPORT epoll-targets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
  install(TARGETS epoll_static epoll
          EXPORT epoll-targets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(FILES include/epoll/epoll.h include/epoll/uepoll.h include/epoll/wepoll.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/epoll)
