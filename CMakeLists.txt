# vtest builds as a single translation unit with no dependencies; this exists
# for the tests, for installing it once per machine, and so vtest can be run
# against itself.
#
#   cmake -S . -B build && ctest --test-dir build
#   cmake --build build && cmake --install build --prefix ~/.local
cmake_minimum_required(VERSION 3.16)
project(vtest C)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The exact commit, not a hand-bumped number: an installed vtest is shared by
# every repo on the machine, and `vtest --version` is how you tell which one.
execute_process(COMMAND git describe --tags --always --dirty
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE VTEST_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)

add_executable(vtest vtest.c)
target_compile_options(vtest PRIVATE -std=c11 -Wall -Wextra)
if(VTEST_VERSION)
  target_compile_definitions(vtest PRIVATE VTEST_VERSION="${VTEST_VERSION}")
endif()

install(TARGETS vtest)
install(PROGRAMS loc.sh DESTINATION bin RENAME vtest-loc)

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

