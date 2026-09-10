add_library(poker_eval
  src/ref_evaluator.cpp
  src/fast_evaluator.cpp)
add_library(poker::eval ALIAS poker_eval)

target_include_directories(poker_eval PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(poker_eval PUBLIC cxx_std_20)
target_link_libraries(poker_eval PRIVATE poker::warnings)

if(POKER_BUILD_BENCH)
  find_package(nanobench CONFIG REQUIRED)
  add_executable(poker_eval_bench bench/bench_eval.cpp)
  target_link_libraries(poker_eval_bench PRIVATE poker::eval nanobench::nanobench poker::warnings)
endif()
