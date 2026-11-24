cmake_minimum_required(VERSION 4.2)
project(Ameobatu)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
enable_testing()
add_subdirectory(src)
