find_package(doctest CONFIG REQUIRED)

add_executable(poker_tests
  test_main.cpp
  unit/eval_test.cpp
  unit/core_test.cpp
  property/eval_diff_test.cpp
  property/playout_test.cpp)
target_link_libraries(poker_tests PRIVATE poker::core doctest::doctest poker::warnings)
target_include_directories(poker_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME poker_tests COMMAND poker_tests)
