# vtest roadmap

Fixes first, install last: installing a tool that can report a false pass
spreads the false pass to every repo that uses it, and one shared binary cannot
carry Vayu-specific code.

## Phase 0 — parser tests

- [ ] `tests/parse_test.c`: ctest result lines (Passed, Failed, `***Not Run`,
      the two-space `Test  #1`), pytest `-v` lines, `load_config` (comments,
      empty keys, several suites). Registered in CMake, so `unit` and
      `unit-asan` both run it.

## Phase 1 — correctness

- [ ] A crashed run must never be ALL PASSED. After a run, a case still pending
      or running is a FAIL, and so is a non-zero exit with no failure parsed.
- [ ] Ctrl-C orphans the build/test process group. Clear `ISIG` in raw mode so
      it arrives as byte 3, which `stream_exec` already turns into a group kill.
- [ ] pytest with no `filter` passes NULL to `%s` (`pytest '(null)'`).
- [ ] Strict config and CLI: an unknown adapter, key or flag is an error.
      Add `--help`. This is also what catches version skew between an
      installed vtest and a newer `vtest.conf`.

## Phase 2 — generic

- [ ] A `build =` key for any suite. Delete the hard-coded
      `vayu_sitl_rtos` build; pytest builds nothing by default.
- [ ] Delete `VAYU_SITL_RTOS_BIN` from the pytest run command; `vayu.sh`
      exports it.
- [ ] Vayu (its own repo): add the `build =` line to `vtest.conf` and the
      export to `vayu.sh`.

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

- [ ] Vayu `vayu.sh` and NavLink `scripts/vtest.sh`: use `vtest` from PATH if
      installed, else build the submodule (CI keeps working without install).
- [ ] Release v2.0.0 — breaking: a crashed suite fails, pytest builds nothing
      by default, unknown keys are errors. Bump each consumer's pin together
      with its `vtest.conf`.

## Left out on purpose

- .deb / Homebrew — when someone else installs it.
- Dropping the submodule — it is the version pin and the CI fallback. Drop it
  when CI installs vtest itself.
