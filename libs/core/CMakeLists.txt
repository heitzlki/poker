add_library(poker_core src/engine.cpp)
add_library(poker::core ALIAS poker_core)

target_include_directories(poker_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(poker_core
  PUBLIC poker::eval
  PRIVATE poker::warnings)
