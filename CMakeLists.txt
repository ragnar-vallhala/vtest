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

add_executable(vtest vtest.c)
target_compile_options(vtest PRIVATE -std=c11 -Wall -Wextra)

# The exact commit, not a hand-bumped number: an installed vtest is shared by
# every repo on the machine, and `vtest --version` is how you tell which one.
# Read at build time, not configure time, so it follows every commit.
set(VTEST_VERSION_H ${CMAKE_CURRENT_BINARY_DIR}/vtest_version.h)
add_custom_target(vtest_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT=${VTEST_VERSION_H} -P ${CMAKE_CURRENT_SOURCE_DIR}/version.cmake
  BYPRODUCTS ${VTEST_VERSION_H})
add_dependencies(vtest vtest_version)
target_compile_options(vtest PRIVATE -include ${VTEST_VERSION_H})

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

