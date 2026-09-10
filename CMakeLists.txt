cmake_minimum_required(VERSION 3.25)

project(poker
  VERSION 0.1.0
  DESCRIPTION "Multi-variant poker rules engine"
  HOMEPAGE_URL "https://github.com/heitzlki/poker"
  LANGUAGES CXX)

option(POKER_BUILD_TESTS "Build the poker test suite" ${PROJECT_IS_TOP_LEVEL})
option(POKER_BUILD_BENCH "Build the poker benchmarks" ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

add_library(poker_warnings INTERFACE)
add_library(poker::warnings ALIAS poker_warnings)
target_compile_options(poker_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion;-Wshadow;-Wold-style-cast;-Wnon-virtual-dtor>
  $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->)

add_subdirectory(libs/eval)

if(POKER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
