add_executable(poker_cli main.cpp)
set_target_properties(poker_cli PROPERTIES OUTPUT_NAME poker)
target_link_libraries(poker_cli PRIVATE poker::core poker::warnings)

if(POKER_BUILD_TESTS)
  add_test(NAME cli_eval COMMAND poker_cli eval "Ah Kh Qh Jh Th 2c 3c")
  set_tests_properties(cli_eval PROPERTIES PASS_REGULAR_EXPRESSION "straight flush")

  add_test(NAME cli_sim COMMAND poker_cli sim --hands 200 --seed 1 --bots 4)
  set_tests_properties(cli_sim PROPERTIES PASS_REGULAR_EXPRESSION "200 hands")

  add_test(NAME cli_play
    COMMAND bash -c "printf 'f\\nq\\n' | $<TARGET_FILE:poker_cli> play --bots 2 --seed 7")

  add_test(NAME cli_bad_bots COMMAND poker_cli play --bots 99)
  add_test(NAME cli_bad_stakes COMMAND poker_cli play --stakes nope)
  add_test(NAME cli_unknown_command COMMAND poker_cli bogus)
  set_tests_properties(cli_bad_bots cli_bad_stakes cli_unknown_command
    PROPERTIES WILL_FAIL TRUE)

  # Regenerate after deliberate output changes:
  #   printf 'c\nx\nb 30\nf\nq\n' | poker play --bots 2 --seed 7 > tests/golden_play.txt
  add_test(NAME cli_golden
    COMMAND bash -c "printf 'c\\nx\\nb 30\\nf\\nq\\n' | $<TARGET_FILE:poker_cli> play --bots 2 --seed 7 | diff -u ${CMAKE_SOURCE_DIR}/tests/golden_play.txt -")
endif()
