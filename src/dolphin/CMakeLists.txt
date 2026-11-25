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

# Sanitizers: OFF, ON or address (ASan + UBSan), or thread (TSan).
set(DOLPHIN_SANITIZE OFF CACHE STRING "Sanitizer build: OFF, ON/address (ASan + UBSan), thread (TSan)")
set_property(CACHE DOLPHIN_SANITIZE PROPERTY STRINGS OFF ON address thread)

# Memory allocator. The interpreter allocates small values constantly, from
# every thread; glibc malloc contends across threads (cross-thread frees lock
# the owning arena). mimalloc or jemalloc measured 7-15% faster on the METAR
# benchmark. auto = mimalloc if found, then jemalloc, else the system one.
# Sanitizer builds always use the system allocator (the sanitizers replace it).
set(DOLPHIN_ALLOCATOR auto CACHE STRING "Memory allocator: auto, mimalloc, jemalloc, system")
set_property(CACHE DOLPHIN_ALLOCATOR PROPERTY STRINGS auto mimalloc jemalloc system)

add_library(dolphin_core STATIC
  src/lexer.cpp
  src/parser.cpp
  src/ast.cpp
  src/value.cpp
  src/resolver.cpp
  src/runtime.cpp
  src/costmodel.cpp
  src/topology.cpp
  src/sharedprofile.cpp
  src/pcode.cpp
  src/dcl.cpp
  src/verify.cpp
  src/analyzer.cpp
  src/interp.cpp
  src/builtins.cpp
)
target_include_directories(dolphin_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(dolphin_core PUBLIC Threads::Threads)
# shm_open (shared machine profile) lives in librt before glibc 2.34 (e.g.
# Alma/RHEL 8); newer glibc keeps an empty librt, so linking it is harmless.
find_library(DOLPHIN_RT_LIBRARY rt)
if(DOLPHIN_RT_LIBRARY)
  target_link_libraries(dolphin_core PUBLIC ${DOLPHIN_RT_LIBRARY})
endif()
target_compile_features(dolphin_core PUBLIC cxx_std_17)
target_compile_options(dolphin_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dolphin src/main.cpp)
target_link_libraries(dolphin PRIVATE dolphin_core)
target_compile_options(dolphin PRIVATE -Wall -Wextra -Wpedantic)

# Mutation fuzzer for the .dcl loader and verifier (tests/fuzz_dcl.sh runs it).
# Not built by default; most useful in a DOLPHIN_SANITIZE build.
add_executable(dcl_fuzz EXCLUDE_FROM_ALL tests/dcl_fuzz.cpp)
target_link_libraries(dcl_fuzz PRIVATE dolphin_core)
target_compile_options(dcl_fuzz PRIVATE -Wall -Wextra -Wpedantic)

if(DOLPHIN_SANITIZE STREQUAL "thread")
  set(_dolphin_san thread)
elseif(DOLPHIN_SANITIZE)
  set(_dolphin_san address,undefined)
endif()
if(_dolphin_san)
  foreach(t dolphin_core dolphin dcl_fuzz)
    target_compile_options(${t} PRIVATE -fsanitize=${_dolphin_san} -fno-omit-frame-pointer)
    target_link_options(${t} PRIVATE -fsanitize=${_dolphin_san})
  endforeach()
endif()

# Allocator selection (see DOLPHIN_ALLOCATOR above). Linking the library
# replaces malloc/free/new/delete for the whole process.
set(_dolphin_alloc system)
set(_dolphin_alloc_lib "")
if(NOT _dolphin_san AND NOT DOLPHIN_ALLOCATOR STREQUAL "system")
  find_library(DOLPHIN_MIMALLOC_LIBRARY NAMES mimalloc libmimalloc.so.2)
  find_library(DOLPHIN_JEMALLOC_LIBRARY NAMES jemalloc libjemalloc.so.2)
  if(DOLPHIN_ALLOCATOR MATCHES "^(auto|mimalloc)$" AND DOLPHIN_MIMALLOC_LIBRARY)
    set(_dolphin_alloc mimalloc)
    set(_dolphin_alloc_lib ${DOLPHIN_MIMALLOC_LIBRARY})
  elseif(DOLPHIN_ALLOCATOR MATCHES "^(auto|jemalloc)$" AND DOLPHIN_JEMALLOC_LIBRARY)
    set(_dolphin_alloc jemalloc)
    set(_dolphin_alloc_lib ${DOLPHIN_JEMALLOC_LIBRARY})
  elseif(NOT DOLPHIN_ALLOCATOR STREQUAL "auto")
    message(FATAL_ERROR "DOLPHIN_ALLOCATOR=${DOLPHIN_ALLOCATOR} requested but not found")
  endif()
endif()
if(_dolphin_alloc_lib)
  # --no-as-needed: the linker must keep the library even though nothing names
  # it directly; its malloc must win over libc's.
  target_link_libraries(dolphin PRIVATE "-Wl,--no-as-needed" ${_dolphin_alloc_lib} "-Wl,--as-needed")
endif()
target_compile_definitions(dolphin PRIVATE DOLPHIN_ALLOCATOR_NAME="${_dolphin_alloc}")
message(STATUS "Dolphin allocator: ${_dolphin_alloc}")

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
