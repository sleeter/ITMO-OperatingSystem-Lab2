cmake_minimum_required(VERSION 3.29)
project(OS_Lab2)

set(CMAKE_CXX_STANDARD 20)

#set(CMAKE_CXX_FLAGS_DEBUG "-O3 -g -march=native -mtune=native -flto -funroll-loops -fomit-frame-pointer")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

# Dynamic library
add_library(lab2_cache SHARED API/lab2_cache.cpp)

add_executable(writebench writebench.cpp)
target_link_libraries(writebench lab2_cache)
target_include_directories(writebench PRIVATE lab2_cache)

add_executable(emasearchbench emasearchbench.cpp)
target_link_libraries(emasearchbench lab2_cache)
target_include_directories(emasearchbench PRIVATE lab2_cache)