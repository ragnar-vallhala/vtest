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
adapter = ctest          # ctest | pytest
dir     = build_sitl     # ctest: cmake binary dir; pytest: rootdir
filter  = ^tst_          # ctest: -R regex; pytest: subdir
prefix  = test_          # ctest: test name -> build target
open    = 1              # start the group expanded in the TUI
```

Unset keys are empty, which both adapters already read as "no filter" and "no
prefix". Lines starting with `#` are comments.

## Adapters

- **ctest** — discover with `ctest -N`, run with `--output-on-failure`. `prefix`
  maps a test name to its CMake target so a single case builds alone.
- **pytest** — discover with `--collect-only -q`, run with `-v`, from the rootdir
  so nodeids match between the two.
Result lines are parsed as text. No XML, no JSON, no reporting plugins.

## Consumers

Pinned as a submodule by the Vayu flight stack (ctest across firmware, GCS and
SITL; pytest for the headless SDK) and by NavLink (ctest). Changes here affect
both — check `vtest.conf` in each before altering an adapter's contract.

A suite in some other framework registers with ctest rather than getting its own
adapter here: `add_test` runs any command, which is how NavLink's Python tests
and the in-house C frameworks in NavHAL and vaios are all driven through one
adapter.

## License

Apache License 2.0 — see [LICENSE.md](LICENSE.md). Copyright (C) 2026 NAVRobotec
Pvt Ltd.
