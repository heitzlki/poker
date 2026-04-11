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

if(TARGET poker_cli)
  add_executable(poker_tui_test tui_test.cpp)
  target_link_libraries(poker_tui_test PRIVATE poker::warnings)
  find_library(UTIL_LIBRARY util) # forkpty lives in libutil on older glibc
  if(UTIL_LIBRARY)
    target_link_libraries(poker_tui_test PRIVATE ${UTIL_LIBRARY})
  endif()
  add_test(NAME cli_tui COMMAND poker_tui_test $<TARGET_FILE:poker_cli>)
  set_tests_properties(cli_tui PROPERTIES TIMEOUT 60)
endif()
