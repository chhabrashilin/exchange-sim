cmake_minimum_required(VERSION 3.20)
project(exchange_sim VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(EXSIM_NATIVE   "Tune for the build machine (-march=native)" ON)
option(EXSIM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(EXSIM_TSAN     "Build with ThreadSanitizer" OFF)
option(EXSIM_WERROR   "Treat warnings as errors" OFF)

find_package(Threads REQUIRED)

# The engine: header-only, no third-party dependencies.
add_library(exsim INTERFACE)
add_library(exsim::exsim ALIAS exsim)
target_include_directories(exsim INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(exsim INTERFACE cxx_std_20)
target_link_libraries(exsim INTERFACE Threads::Threads)

# Build flags shared by every binary in this repository.
add_library(exsim_flags INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(exsim_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
                                                -Wcast-align -Wimplicit-fallthrough)
  if(EXSIM_WERROR)
    target_compile_options(exsim_flags INTERFACE -Werror)
  endif()
  if(EXSIM_NATIVE AND NOT EXSIM_SANITIZE AND NOT EXSIM_TSAN)
    target_compile_options(exsim_flags INTERFACE -march=native)
  endif()
  if(EXSIM_SANITIZE)
    target_compile_options(exsim_flags INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(exsim_flags INTERFACE -fsanitize=address,undefined)
  endif()
  if(EXSIM_TSAN)
    target_compile_options(exsim_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(exsim_flags INTERFACE -fsanitize=thread)
  endif()
endif()

function(exsim_executable name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE exsim exsim_flags)
endfunction()

exsim_executable(exsim_gen      tools/exsim_gen.cpp)
exsim_executable(exsim_replay   tools/exsim_replay.cpp)
exsim_executable(exsim_pipeline tools/exsim_pipeline.cpp)
exsim_executable(exsim_bench    bench/exsim_bench.cpp)

exsim_executable(exsim_tests
  tests/test_main.cpp
  tests/test_book.cpp
  tests/test_components.cpp
  tests/test_differential.cpp
  tests/test_alloc.cpp)

enable_testing()
add_test(NAME exsim_tests COMMAND exsim_tests)
set_tests_properties(exsim_tests PROPERTIES TIMEOUT 600)  # a corrupted FIFO can loop forever
