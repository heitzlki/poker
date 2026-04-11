find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(pokercore module.cpp)
target_link_libraries(pokercore PRIVATE poker::core poker::warnings)

if(SKBUILD)
  install(TARGETS pokercore LIBRARY DESTINATION .)
endif()

if(POKER_BUILD_TESTS)
  add_test(NAME python_smoke
    COMMAND Python::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/smoke.py)
  set_tests_properties(python_smoke PROPERTIES
    ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:pokercore>")
endif()
