cmake_minimum_required(VERSION 3.0.0)

project(invisibledb)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS on)

# Define functions after add_subdirectory, so they are not overwritten
include(cmake/wall.cmake)

if(INVISIBLEDB_ENABLE_ASAN)
  include(cmake/asan.cmake)
endif()

add_library(invisibledb src/invisibledb.cpp)
set_wall(invisibledb)

add_executable(test src/main.cpp)
target_link_libraries(test invisibledb)
set_wall(test)
