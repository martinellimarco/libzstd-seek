cmake_minimum_required(VERSION 3.0)
project(libzstd-seek C)

set(CMAKE_C_STANDARD 99)

add_library(zstd-seek zstd-seek.c zstd-seek.h)
target_link_libraries(zstd-seek zstd m)

add_subdirectory(examples)
