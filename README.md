# vtest — test orchestrator

A single-file C program that discovers and runs this repo's test suites, either
as an interactive TUI or in batch. Zero dependencies: raw ANSI + termios, no
ncurses, no libraries.

```sh
scripts/vtest.sh          # interactive TUI
scripts/vtest.sh --run    # discover + run everything + report
scripts/vtest.sh --list   # print the catalog without running
```

## Suites here

| Component | Adapter | What it runs |
| --- | --- | --- |
| `codec` | script | every standalone `tests/test_*.py`, one case each |
| `pipeline` | script | `tests/run_tests.py` — the full 7-step run |

`codec` and `pipeline` overlap deliberately: the individual scripts give a fast
selective loop, while the pipeline adds the steps no single script covers —
regeneration from `dialect.json`, compiling the C test, and the C↔Python parity
checks over CRC_EXTRA and wire size.

## Adapters

Three, selected per component in the `comps[]` table at the top of `vtest.c`:

- **ctest** — discover with `ctest -N`, run with `--output-on-failure`.
- **pytest** — discover with `--collect-only -q`, run with `-v`.
- **script** — standalone executables that print PASS/FAIL and exit non-zero.
  `filter` is a shell command listing one case name per line, `tprefix` is what
  each name is appended to, and `build` is an optional prerequisite command run
  before the cases (here: regenerating the codec, without which a clean tree
  fails every case for a reason unrelated to the test). The run wraps each case to emit `<name> PASSED` /
  `<name> FAILED`, which is the shape `pytest -v` produces — so it reuses that
  parser rather than adding a second one to keep in step.

Both of this repo's suites use **script**: NavLink's tests are plain `python3`
files with no framework, because the wire contract three codebases generate from
has to be testable anywhere without a package install.

## Note on provenance

This orchestrator came from the Vayu flight stack, where it drives ctest and
pytest suites across firmware, GCS and SITL. The ctest and pytest adapters are
kept here unused so the tool stays usable by the other repos that consume this
one. If it is going to live in two places it needs an owner — otherwise the
copies drift.
