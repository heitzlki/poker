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
endif()
