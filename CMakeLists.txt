message("======================================")
message("Project Name   : epoll")
message("Author  Name   : CandyMi")
message("Author  Email  : 869646063@qq.com")
message("Author  Github : github.com/CandyMi")
message("======================================")

# 最低版本号
cmake_minimum_required(VERSION 3.10)

# 项目名称
project("epoll")

set(CMAKE_C_STANDARD 99)

message("-- Detecting OS: '${CMAKE_SYSTEM_NAME}'")

add_executable(main main.c)
if(CMAKE_SYSTEM_NAME MATCHES "Windows")
  # MESSAGE("Windows")
  file(GLOB epoll_src wepoll.c)
  add_library(epoll_dynamic SHARED ${epoll_src})
  set_target_properties(epoll_dynamic PROPERTIES OUTPUT_NAME "epoll" PREFIX "" )
  add_library(epoll_static STATIC ${epoll_src})
  set_target_properties(epoll_static PROPERTIES OUTPUT_NAME "epoll" PREFIX "")
  target_link_libraries(main PRIVATE epoll_static)
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
  file(GLOB epoll_src epoll.c)
  add_library(epoll_dynamic SHARED ${epoll_src})
  set_target_properties(epoll_dynamic PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
  add_library(epoll_static STATIC ${epoll_src})
  set_target_properties(epoll_static PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
  target_link_libraries(main PRIVATE epoll_static)
elseif(CMAKE_SYSTEM_NAME MATCHES "BSD|Darwin")
  # MESSAGE("*BSD|Darwin")
  file(GLOB epoll_src kepoll.c)
  add_library(epoll_dynamic SHARED ${epoll_src})
  set_target_properties(epoll_dynamic PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
  add_library(epoll_static STATIC ${epoll_src})
  set_target_properties(epoll_static PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
  target_link_libraries(main PRIVATE epoll_static)
else()
  # MESSAGE("Unix-Like")
  file(GLOB epoll_src uepoll.c)
  add_library(epoll_dynamic SHARED ${epoll_src})
  set_target_properties(epoll_dynamic PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
  add_library(epoll_static STATIC ${epoll_src})
  set_target_properties(epoll_static PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
  target_link_libraries(main PRIVATE epoll_static)
endif()

find_file(EPOLL_ATOMIC NAMES stdatomic.h)
if (EPOLL_ATOMIC)
  target_compile_definitions(epoll_dynamic PRIVATE EPOLL_USE_ATOMIC=1)
  target_compile_definitions(epoll_static PRIVATE EPOLL_USE_ATOMIC=1)
endif(EPOLL_ATOMIC)

# ── 安装规则 ──────────────────────────────
include(GNUInstallDirs)

install(TARGETS epoll_static epoll_dynamic
        EXPORT epoll-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES epoll.h uepoll.h wepoll.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/epoll)
