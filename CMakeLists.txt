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

# Line + branch coverage of vtest.c, from every test below -- the unit tests
# include it, and the TUI test drives the real binary through a pty. -O0 so
# gcov's branches are the source's branches, not the optimiser's.
#   cmake -S . -B build_cov -DVTEST_COVERAGE=ON && cmake --build build_cov
#   ctest --test-dir build_cov && cmake --build build_cov --target coverage
option(VTEST_COVERAGE "Instrument vtest and the tests for gcov" OFF)
if(VTEST_COVERAGE)
  add_compile_options(--coverage -O0)
  add_link_options(--coverage)
  find_program(GCOVR gcovr REQUIRED)
  # Run after ctest. Fails under the gates in gcovr.cfg; the HTML report is
  # left in <build>/coverage/ either way.
  add_custom_target(coverage
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
    COMMAND ${GCOVR} --config ${CMAKE_SOURCE_DIR}/gcovr.cfg
            --root ${CMAKE_SOURCE_DIR} --object-directory ${CMAKE_BINARY_DIR}
            --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
            --txt ${CMAKE_BINARY_DIR}/coverage/summary.txt
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
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
foreach(t pane_scroll parse tui)
  add_executable(test_${t} tests/${t}_test.c)
  target_compile_options(test_${t} PRIVATE -std=c11 -Wall -Wextra -Werror -UNDEBUG)
  if(VTEST_SANITIZE)
    target_compile_options(test_${t} PRIVATE -fsanitize=address,undefined
                                             -fno-omit-frame-pointer)
    target_link_options(test_${t} PRIVATE -fsanitize=address,undefined)
  endif()
  if(VTEST_COVERAGE)
    target_compile_definitions(test_${t} PRIVATE VTEST_COVERAGE)
  endif()
  add_test(NAME test_${t} COMMAND test_${t})
endforeach()

# The real binary against tests/fixture, TUI included (through a pty). The
# fixture's pytest suite needs pytest.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME e2e COMMAND Python3::Interpreter
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_test.py $<TARGET_FILE:vtest>)
endif()

