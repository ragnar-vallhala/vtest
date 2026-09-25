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
foreach(t pane_scroll parse)
  add_executable(test_${t} tests/${t}_test.c)
  target_compile_options(test_${t} PRIVATE -std=c11 -Wall -Wextra -Werror -UNDEBUG)
  if(VTEST_SANITIZE)
    target_compile_options(test_${t} PRIVATE -fsanitize=address,undefined
                                             -fno-omit-frame-pointer)
    target_link_options(test_${t} PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME test_${t} COMMAND test_${t})
endforeach()

