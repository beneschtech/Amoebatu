cmake_minimum_required(VERSION 4.2)
project(Ameobatu)

set(CMAKE_CXX_STANDARD 20)
set(AMOEBATU_VERSION 1.0.0)
find_package(PkgConfig REQUIRED)
enable_testing()
add_compile_definitions(AMOEBATU_VERSION="${AMOEBATU_VERSION}")
add_subdirectory(src)
