# Dolphin — the Amoebatu language. Builds as src/dolphin inside Amoebatu
# (inherits the parent's C++ standard) or standalone.
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(dolphin LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
  endif()
  enable_testing()
endif()

option(DOLPHIN_SANITIZE "Build Dolphin with ASan + UBSan" OFF)

add_library(dolphin_core STATIC
  src/lexer.cpp
  src/parser.cpp
  src/ast.cpp
  src/value.cpp
  src/analyzer.cpp
  src/interp.cpp
  src/builtins.cpp
)
target_include_directories(dolphin_core PUBLIC src)
target_compile_features(dolphin_core PUBLIC cxx_std_17)
target_compile_options(dolphin_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dolphin src/main.cpp)
target_link_libraries(dolphin PRIVATE dolphin_core)
target_compile_options(dolphin PRIVATE -Wall -Wextra -Wpedantic)

if(DOLPHIN_SANITIZE)
  foreach(t dolphin_core dolphin)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

add_test(NAME dolphin_golden
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:dolphin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests)

# API documentation: `cmake --build . --target dolphin_docs`
# Output: src/dolphin/build-docs/html/index.html (git-ignored).
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
  add_custom_target(dolphin_docs
    COMMAND ${DOXYGEN_EXECUTABLE} Doxyfile
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Dolphin API docs in src/dolphin/build-docs/html"
    VERBATIM)
endif()
