# Run on every build (cmake -P, from CMakeLists.txt): writes OUT with the git
# describe of SRC, and touches it only when that changed -- so a new commit or
# a dirty tree shows in --version without re-running cmake, and an unchanged
# version recompiles nothing.
execute_process(COMMAND git describe --tags --always --dirty
                WORKING_DIRECTORY ${SRC}
                OUTPUT_VARIABLE v OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT v)
  set(v dev)
endif()
set(text "#define VTEST_VERSION \"${v}\"\n")
if(EXISTS ${OUT})
  file(READ ${OUT} old)
endif()
if(NOT old STREQUAL text)
  file(WRITE ${OUT} "${text}")
endif()
