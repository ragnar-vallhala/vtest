# vtest — test orchestrator

A single-file C program that discovers and runs a repo's test suites, as an
interactive TUI or in batch. Zero dependencies: raw ANSI + termios, no ncurses,
no libraries, no build system.

```sh
cc -std=c11 -O2 -Wall -Wextra vtest.c -o build/vtest

build/vtest              # interactive TUI
build/vtest --run        # discover + run everything + report
build/vtest --list       # print the catalog without running
build/vtest --conf PATH  # a config other than ./vtest.conf
```

Run it from the root of the repo being tested. Suites are built on demand when a
case is selected, so a cold tree costs nothing until you actually run something.

## vtest.conf

The catalog is **not** compiled in — vtest is pinned as a submodule by more than
one repo, and each declares its own suites in a `vtest.conf` at its root. One
block per suite:

```ini
[sitl]
adapter = ctest          # ctest | pytest | script
dir     = build_sitl     # ctest: cmake binary dir; pytest: rootdir; script: cwd
filter  = ^tst_          # ctest: -R regex; pytest: subdir; script: lists case names
prefix  = test_          # ctest: name -> target; script: command each name follows
build   = make gen       # script only: prerequisite run before the cases
open    = 1              # start the group expanded in the TUI
```

Unset keys are empty, which every adapter already reads as "no filter", "no
prefix", "nothing to build". Lines starting with `#` are comments.

## Adapters

- **ctest** — discover with `ctest -N`, run with `--output-on-failure`. `prefix`
  maps a test name to its CMake target so a single case builds alone.
- **pytest** — discover with `--collect-only -q`, run with `-v`, from the rootdir
  so nodeids match between the two.
- **script** — standalone executables that print PASS/FAIL and exit non-zero.
  `filter` is a shell command listing one case name per line, `prefix` is what
  each name is appended to. The run wraps each case to emit `<name> PASSED` /
  `<name> FAILED`, which is the shape `pytest -v` produces — so it reuses that
  parser rather than adding a second one to keep in step.

Result lines are parsed as text. No XML, no JSON, no reporting plugins.

## Consumers

Pinned as a submodule by the Vayu flight stack (ctest + pytest suites across
firmware, GCS and SITL) and by NavLink (script suites). Changes here affect both
— check `vtest.conf` in each before altering an adapter's contract.
