# A repo for e2e_test.py to point vtest at: every ctest verdict, no compiler.
cmake_minimum_required(VERSION 3.16)
project(fixture NONE)
enable_testing()
add_test(NAME ok COMMAND true)
add_test(NAME bad COMMAND sh -c "echo boom; echo second line; exit 1")
add_test(NAME skipped COMMAND sh -c "exit 77")
set_tests_properties(skipped PROPERTIES SKIP_RETURN_CODE 77)
add_custom_target(ok)
