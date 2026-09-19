# vtest builds as a single translation unit with no dependencies; this exists
# for the tests, and so vtest can be run against itself.
#
#   cmake -S . -B build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(vtest C)

add_executable(vtest vtest.c)
target_compile_options(vtest PRIVATE -std=c11 -Wall -Wextra)

option(VTEST_SANITIZE "Build with ASan + UBSan" OFF)

enable_testing()

# The unit under test is included, not linked: vtest is one file of statics, so
# a test TU includes it with main() renamed away. -UNDEBUG because every check
# is an assert() and a release build would compile them all out.
add_executable(test_log_scroll tests/log_scroll_test.c)
target_compile_options(test_log_scroll
                       PRIVATE -std=c11 -Wall -Wextra -Werror -UNDEBUG)
if(VTEST_SANITIZE)
  target_compile_options(test_log_scroll PRIVATE -fsanitize=address,undefined
                                                 -fno-omit-frame-pointer)
  target_link_options(test_log_scroll PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME test_log_scroll COMMAND test_log_scroll)
