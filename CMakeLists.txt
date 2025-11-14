message("======================================")
message("Project Name   : epoll")
message("Author  Name   : CandyMi")
message("Author  Email  : 869646063@qq.com")
message("Author  Github : github.com/CandyMi")
message("======================================")

# 最低版本号
cmake_minimum_required(VERSION 2.8...3.13)

# 项目名称
project("Epoll")

MESSAGE("-- Detecting OS: '${CMAKE_SYSTEM_NAME}'")

add_executable(main main.c)
if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  # MESSAGE("Windows")
  file(GLOB epoll_src wepoll.c)
  add_library(epoll-dynamic SHARED ${epoll_src})
  set_target_properties(epoll-dynamic PROPERTIES OUTPUT_NAME "epoll" PREFIX "" )
  add_library(epoll-static STATIC ${epoll_src})
  set_target_properties(epoll-static PROPERTIES OUTPUT_NAME "epoll" PREFIX "")
  target_link_libraries(main PRIVATE epoll-static)
elseif(NOT ${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  # MESSAGE("Unix-Like")
  file(GLOB epoll_src uepoll.c)
  add_library(epoll-dynamic SHARED ${epoll_src})
  set_target_properties(epoll-dynamic PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
  add_library(epoll-static STATIC ${epoll_src})
  set_target_properties(epoll-static PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
  target_link_libraries(main PRIVATE epoll-static)
endif()
