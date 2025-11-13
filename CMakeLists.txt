message("======================================")
message("Project Name   : epoll")
message("Author  Name   : CandyMi")
message("Author  Email  : 869646063@qq.com")
message("Author  Github : github.com/CandyMi")
message("======================================")

# 最低版本号
cmake_minimum_required(VERSION 2.8...3.13)

# 项目名称
project("Epoll" C)

if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  file(GLOB epoll_src wepoll.c)
else()
  file(GLOB epoll_src uepoll.c)
endif()

# 静态库
add_library(epoll-static STATIC ${epoll_src})
# 动态库
add_library(epoll-dynamic SHARED ${epoll_src})

if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  set_target_properties(epoll-dynamic PROPERTIES OUTPUT_NAME "epoll" PREFIX "" )
  set_target_properties(epoll-static PROPERTIES OUTPUT_NAME "epoll" PREFIX "")
else()
  set_target_properties(epoll-dynamic PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
  set_target_properties(epoll-static PROPERTIES OUTPUT_NAME "epoll" PREFIX "lib")
endif()

# 示例代码
add_executable(main main.c)
target_link_libraries(main PRIVATE epoll-static)