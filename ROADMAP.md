# vtest roadmap

Fixes first, install last: installing a tool that can report a false pass
spreads the false pass to every repo that uses it, and one shared binary cannot
carry Vayu-specific code.

## Phase 0 — parser tests

- [x] `tests/parse_test.c`: ctest result lines (Passed, Failed, `***Not Run`,
      the two-space `Test  #1`), pytest `-v` lines, `load_config` (comments,
      empty keys, several suites). Registered in CMake, so `unit` and
      `unit-asan` both run it.

## Phase 1 — correctness

- [x] A crashed run must never be ALL PASSED. After a run, a case still pending
      or running is a FAIL, and so is a non-zero exit with no failure parsed.
- [x] Ctrl-C orphans the build/test process group. The SIGINT/SIGTERM handler
      kills the child's group, in every mode (`--run` in CI too).
- [x] pytest with no `filter` passed NULL to `%s`; it now defaults to `.`.
- [x] Strict config and CLI: an unknown adapter, key or flag is an error.
      Add `--help`. This is also what catches version skew between an
      installed vtest and a newer `vtest.conf`.

## Phase 2 — generic

- [x] A `build =` key for any suite. Deleted the hard-coded
      `vayu_sitl_rtos` build; pytest builds nothing by default.
- [x] Deleted `VAYU_SITL_RTOS_BIN` from the pytest run command; whoever
      launches vtest exports what its tests need.
- [x] Consumers: nothing to change. Neither Vayu nor NavLink declares a pytest
      suite, so the removed code was dead for both.

## Phase 3 — limits

- [ ] Rows grow instead of stopping at 256.
- [ ] Failure detail kept whole, up to a cap of about 64 KB (currently ~1400 B).
- [ ] pytest failures get detail, taken from its `FAILURES` section.
- [ ] A command too long for its buffer is an error, not a truncated command.
- [ ] Escape regex characters in `-R '^name$'`; look for `Passed`/`sec` only
      after the leader dots.
- [ ] UI: `q` stops a whole run-all, Esc alone does not block, `d`
      re-discovers.

## Phase 4 — cleanup

- [ ] Delete the unused interactive branches of `render()` and `rule()`/`VT_W`.
- [ ] One copy of the counting and row formatting, not two. Fix the stale
      catalog comment.

## Phase 5 — install once

- [ ] `project(vtest VERSION 2.0.0)`, `install(TARGETS vtest)`,
      `install(PROGRAMS loc.sh RENAME vtest-loc)`, `--version`.

      cmake -S . -B build && cmake --build build && cmake --install build --prefix ~/.local

- [ ] Consumers, each in its own repo:

      | repo    | launcher (prefer `vtest` on PATH, else build) | `[loc]` -> `vtest-loc` | pin |
      |---------|-----------------------------------------------|------------------------|-----|
      | vayu    | `tools/scripts/vayu.sh` `build_vtest`/`run_vtest` | `vtest/loc.sh`      | `vtest` |
      | navlink | `scripts/vtest.sh`                            | `vtest/loc.sh`         | `vtest` |
      | vaios   | `tools/vtest.sh` (keep its `srctree` export)  | `extern/vtest/loc.sh`  | `extern/vtest` |

      NavHAL does not use vtest (`navtest` is its own framework). vayu-navigator
      has no vtest, but `tools/dev/run_clang_tidy.sh` still lists
      `vtest/vtest.c` -- a stale line to delete. vayu's own navlink submodule
      carries a second vtest pin, bumped by bumping navlink.
- [ ] Release v2.0.0 — breaking: a crashed suite fails, pytest builds nothing
      by default, unknown keys are errors. Bump each consumer's pin together
      with its `vtest.conf`.

## Left out on purpose

- .deb / Homebrew — when someone else installs it.
- Dropping the submodule — it is the version pin and the CI fallback. Drop it
  when CI installs vtest itself.
