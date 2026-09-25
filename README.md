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
build/vtest --help
```

The TUI has three panes — **tests**, **detail**, **log**. `←`/`→` move focus
between them and `↑`/`↓` scroll whichever holds focus, so one pair of keys
drives all three; `PgUp`/`PgDn` page the focused pane. The focused pane's rule
brightens, and the footer names it.

`space` expands a suite, `r` runs the selection, `a` runs everything, `f` shows
only failures, `q` quits.

The log follows live output until you scroll back, then holds position as new
lines arrive — so an error stays readable while a suite is still running. The
detail pane holds a failure's whole captured output rather than the three lines
that fit, and resets to the top when the selection moves.

Run it from the root of the repo being tested. Suites are built on demand when a
case is selected, so a cold tree costs nothing until you actually run something.

## vtest.conf

The catalog is **not** compiled in — vtest is pinned as a submodule by more than
one repo, and each declares its own suites in a `vtest.conf` at its root. One
block per suite:

```ini
[sitl]
adapter = ctest          # ctest | pytest | check
dir     = build_sitl     # ctest: cmake binary dir; pytest: rootdir
filter  = ^tst_          # ctest: -R regex; pytest: subdir
prefix  = test_          # ctest: test name -> build target
cmd     = make lint      # check: the command whose exit status is the verdict
configure = cmake -S sim/host -B build_san -DSAN=ON   # run before the build
build   = make sim          # replaces the default build (see below)
open    = 1              # start the group expanded in the TUI
```

Unset keys are empty: "no filter" and "no prefix" (for pytest, no filter means
the whole rootdir). Lines starting with `#` are comments. An unknown adapter or
key is an error, not a default -- it is most likely a key from a newer vtest,
and ignoring it would run the suite some other way than the conf asks.

## Adapters

- **ctest** — discover with `ctest -N`, run with `--output-on-failure`. `prefix`
  maps a test name to its CMake target so a single case builds alone.
- **pytest** — discover with `--collect-only -q`, run with `-v`, from the rootdir
  so nodeids match between the two. Builds nothing unless the suite sets `build =` —
  e.g. integration tests that drive a compiled simulator. Environment those
  tests need (a binary's path) is exported by whatever launches vtest.
- **check** — a whole-repo gate that is one command and one verdict: a linter, a
  formatter, a coverage ratchet, a line count. `cmd` is the command; its exit
  status is the result and its output goes to the log. Modelled as a suite with
  one case, so the tree, the run loop and the batch report handle it unchanged.

Result lines are parsed as text. No XML, no JSON, no reporting plugins.

## Sanitizer and coverage suites

These need no adapter of their own — they are the same cases in a differently
configured build dir:

```ini
[sitl-asan]
adapter = ctest
dir     = build_san
configure = cmake -S sim/host -B build_san -DVAYU_SANITIZE=ON
```

`configure` runs before the build, and again if discovery finds nothing — so a
build dir that has never existed is created rather than reported as an empty
suite. A suite that discovers no cases counts as a **failure** in `--run`: it
means the suite did not run, which must never read as a pass. The same goes for
a case the runner never reported (it crashed, or never got there), and for a
runner that exits non-zero while every parsed case passed.

## loc.sh

`loc.sh [dir ...]` counts lines of source by extension, skipping build dirs,
`.git` and vendored trees. Always exits 0 — it is a report, not a gate — so it
is declared as a `check` whose value is its output.

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

## Testing vtest

```sh
cmake -S . -B build && ctest --test-dir build
build/vtest --run          # or run vtest against itself
```

vtest is one file of statics, so a test reaches the unit under test by
including it with `main()` renamed away — no header extraction for logic that
is not a library, and no test scaffolding in the production file.
