# vtest — Vayu test orchestrator (PROTOTYPE)

A **repo-root, cross-component test orchestrator**: one native C binary that
discovers, runs, and presents *every* test suite in the monorepo as a single
interactive TUI. It replaces the orchestration logic in
`tools/scripts/vayu.sh test` — `vayu.sh test` builds `vtest` and launches it.

Status: **ctest adapter is REAL** — `vtest` discovers and runs the two ctest
suites `vayu.sh test` drives (`sitl` → `build_sitl`, `gcs` → `navigator/build
-R ^tst_`) via real `ctest` calls, and parses the results. The pytest (headless)
and native-C (firmware host) adapters are the next step; the TUI/result model
already supports them. Run from the repo root.

## What it is (and is NOT)

- **IS** a top-level orchestrator that sits *beside* the components
  (`firmware/`, `navigator/`, `sim/`) and aggregates their suites into one view.
- **NOT** a place where tests live. Module-specific tests stay where they are
  and keep their native runners:
  - `firmware/tests/host/*`   — firmware host unit tests (native C)
  - `sim/host/tests/test_*.c` — SITL suite (ctest, 15 cases)
  - `navigator/tests/tst_*`   — GCS unit tests (QtTest)
  - `navigator/headless-sdk/tests/` — integration tests (pytest)

vtest knows the **catalog** of these suites and how to invoke each, then unifies
the results. The component build systems remain the source of truth for *how* a
suite is built and run; vtest is the front-end and aggregator.

## Why a root C orchestrator instead of the shell

- **One interactive surface over heterogeneous runners.** ctest, QtTest, and
  pytest each have their own output. vtest drives all of them, parses their
  results, and shows one navigable tree — run a single case, a component, or
  everything; drill into any failure's message + `file:line`. The shell can only
  stream batch logs (today's `vayu.sh test` experience).
- **Lives at root because its scope is the whole repo.** It is a sibling of the
  components it orchestrates, not owned by any one of them.
- **Self-contained, zero deps** (raw ANSI + termios, no ncurses) — same
  no-vendor-lib ethos as the firmware.
- **TUI and CI share one engine.** `--ci` / `--demo` give deterministic,
  non-interactive output (and later JSON) so CI and the coverage ratchet consume
  exactly what the TUI shows.

## Architecture

```
                 ┌─────────────────────────── vtest (root) ───────────────────────────┐
                 │  catalog  →  runner adapters  →  result model  →  TUI / --ci output │
                 └──────────────┬─────────────┬──────────────┬──────────────┬──────────┘
                                │             │              │              │
                       firmware-host        sitl            gcs          headless
                        (native C)        (ctest)         (qtest)        (pytest)
                   firmware/tests/host  sim/host/tests  navigator/tests  headless-sdk/tests
```

- **catalog** — the static list of components + how to build/invoke each
  (a small table now; later read from a `vtest.toml`-ish manifest or probed).
- **runner adapters** — per-kind: exec `ctest --test-dir … -R name`, `pytest -k`,
  or call a registered native C case directly; capture stdout + exit + timing.
- **result model** — uniform `{component → case → status/time/message}` the TUI
  and CI writers both render.
- **front-ends** — interactive TUI (termios) and `--ci` batch (no termios).

## Build / run

```
gcc -std=c11 -Wall -Wextra vtest/vtest.c -o /tmp/vtest    # single file, no deps
/tmp/vtest          # interactive TUI (run from repo root)
/tmp/vtest --run    # non-interactive: discover + run all + report, exit 0/1 (CI)
/tmp/vtest --list   # discover + print the catalog, don't run
```

Target integration: `vayu.sh test` → CMake builds `vtest` (+ links the native
firmware-host cases) → runs it (TUI when interactive, `--ci` under CI). The
ctest/qtest/pytest components are invoked by vtest as child processes.

## Keys

`↑/↓` move · `space` expand/collapse component · `r` run selected ·
`a` run all · `f` show only failed · `q` quit.
