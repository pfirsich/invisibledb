cmake_minimum_required(VERSION 3.0.0)

project(netmemsync)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS on)

add_compile_definitions(NOMINMAX _USE_MATH_DEFINES) # Windows is trash

# Define functions after add_subdirectory, so they are not overwritten
include(cmake/wall.cmake)

if(NETMEMSYNC_ENABLE_ASAN)
  include(cmake/asan.cmake)
endif()

set(SRC
  main.cpp
)
list(TRANSFORM SRC PREPEND src/)

add_executable(netmemsync ${SRC})
target_include_directories(netmemsync PUBLIC include)
set_wall(netmemsync)
