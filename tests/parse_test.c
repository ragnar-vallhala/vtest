/*
 * Copyright (C) 2026 NAVRobotec Pvt Ltd
 * Author: Ragnar Vallhala
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file parse_test.c
 * @brief The text parsers: ctest and pytest result lines, and vtest.conf.
 *
 * Every verdict vtest reports comes out of these, and they read another
 * program's output as plain text -- so a misread line is a wrong result, not
 * a crash. Same include trick as pane_scroll_test.c.
 */
#define main vtest_main
#include "../vtest.c"
#undef main

/* load_config is called once per vtest process and never freed; this test
 * calls it a dozen times. Turned off here rather than through ASAN_OPTIONS,
 * which would replace the options vtest itself runs the suite with. */
int __lsan_is_turned_off(void);
int __lsan_is_turned_off(void) { return 1; }

static int checks = 0;
#define CHECK(c, what)                                                         \
  do {                                                                         \
    checks++;                                                                  \
    if (!(c)) {                                                                \
      printf("  FAIL %s\n", what);                                             \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int ctest_lines(void) {
  char name[VT_NAME];
  status_t st;
  float ms;

  CHECK(parse_discovery_line("  Test  #1: safety_phase2", name, sizeof name),
        "discovery: two-space single-digit id");
  CHECK(!strcmp(name, "safety_phase2"), "discovery: name");
  CHECK(parse_discovery_line("  Test #12: tst_mixer", name, sizeof name) &&
            !strcmp(name, "tst_mixer"),
        "discovery: two-digit id");
  CHECK(!parse_discovery_line("Total Tests: 3", name, sizeof name),
        "discovery: the total is not a test");

  CHECK(parse_result_line(" 1/15 Test  #1: foo ..........   Passed    0.12 sec",
                          name, sizeof name, &st, &ms),
        "result: passed line");
  CHECK(!strcmp(name, "foo") && st == ST_PASS, "result: foo passed");
  CHECK(ms > 119.0f && ms < 121.0f, "result: 0.12 sec is 120 ms");

  CHECK(parse_result_line("2/15 Test #10: bar ...***Failed    1.50 sec", name,
                          sizeof name, &st, &ms) &&
            !strcmp(name, "bar") && st == ST_FAIL,
        "result: failed line");
  CHECK(parse_result_line("3/15 Test #11: baz ...***Not Run   0.00 sec", name,
                          sizeof name, &st, &ms) &&
            st == ST_FAIL,
        "result: not run is a failure");
  CHECK(parse_result_line("4/15 Test #12: qux ....   Skipped   0.00 sec", name,
                          sizeof name, &st, &ms) &&
            st == ST_SKIP,
        "result: skipped line");
  CHECK(!parse_result_line("  Test  #1: foo", name, sizeof name, &st, &ms),
        "result: a bare discovery line is not a result");
  return 0;
}

static int pytest_lines(void) {
  tcase_t cases[2] = {{.name = "tests/a.py::test_ok"},
                      {.name = "tests/a.py::test_bad[x-1]"}};
  comp_t c = {.cases = cases, .ncases = 2};
  int failed = 0;

  pytest_parse_line(&c, "tests/a.py::test_ok PASSED     [ 50%]", &failed);
  CHECK(cases[0].status == ST_PASS && failed == 0, "pytest: passed");
  pytest_parse_line(&c, "tests/a.py::test_bad[x-1] FAILED  [100%]", &failed);
  CHECK(cases[1].status == ST_FAIL && failed == 1,
        "pytest: parametrised id failed");
  pytest_parse_line(&c, "FAILED tests/a.py::test_bad[x-1] - assert 0",
                    &failed);
  CHECK(failed == 1, "pytest: the summary line is not a second failure");
  return 0;
}

/* Load `text` as a vtest.conf; returns what load_config returns. */
static int load_text(const char *text) {
  char path[] = "/tmp/vtest_parse_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text))
    return -99;
  close(fd);
  int n = load_config(path);
  unlink(path);
  return n;
}

static int config(void) {
  const char *text = "# a comment\n"
                     "[sitl]\n"
                     "adapter = ctest\n"
                     "dir     = build_sitl\n"
                     "filter  = ^tst_\n"
                     "prefix  =\n"
                     "open    = 1\n"
                     "\n"
                     "[sdk]\r\n"
                     "adapter = pytest\r\n"
                     "dir = sdk\r\n"
                     "filter = tests\r\n"
                     "[lint]\n"
                     "  adapter = check\n"
                     "  cmd     = make lint  \n";
  int n = load_text(text);
  CHECK(n == 3 && NCOMPS == 3, "config: three suites");
  CHECK(!strcmp(comps[0].name, "sitl") && comps[0].adapter == AD_CTEST,
        "config: sitl is ctest");
  CHECK(!strcmp(comps[0].test_dir, "build_sitl"), "config: dir");
  CHECK(!strcmp(comps[0].filter, "^tst_"), "config: filter");
  CHECK(!strcmp(comps[0].tprefix, ""), "config: empty prefix reads as \"\"");
  CHECK(comps[0].expanded == 1, "config: open");
  CHECK(comps[1].adapter == AD_PYTEST && !strcmp(comps[1].test_dir, "sdk"),
        "config: CRLF lines");
  CHECK(comps[2].adapter == AD_CHECK && !strcmp(comps[2].cmd, "make lint"),
        "config: indented keys, trailing spaces trimmed");
  CHECK(!strcmp(comps[2].test_dir, "."), "config: dir defaults to .");
  CHECK(load_config("/nonexistent/vtest.conf") == -1, "config: missing file");

  /* Strict: anything not understood is an error, not a silent default. */
  CHECK(load_text("[a]\nadapter = pytset\n") == -2, "config: unknown adapter");
  CHECK(load_text("[a]\nbuidl = make\n") == -2, "config: unknown key");
  CHECK(load_text("adapter = ctest\n") == -2, "config: key before a suite");
  CHECK(load_text("[a\n") == -2, "config: unterminated suite name");
  CHECK(load_text("[a]\njunk\n") == -2, "config: line without =");

  CHECK(load_text("[py]\nadapter = pytest\nbuild = make sim\n") == 1 &&
            !strcmp(comps[0].build, "make sim"),
        "config: build key");
  CHECK(load_text("[py]\nadapter = pytest\n") == 1 &&
            !strcmp(comps[0].filter, "."),
        "config: pytest with no filter collects the whole rootdir");
  return 0;
}

/* The default build per adapter, and `build =` replacing it. */
static int builds(void) {
  char b[512];
  comp_t c = {.adapter = AD_CTEST, .test_dir = "bd", .tprefix = "test_"};
  CHECK(!strcmp(build_cmd(b, sizeof b, &c, NULL), "cmake --build 'bd' 2>&1"),
        "build: ctest suite");
  CHECK(!strcmp(build_cmd(b, sizeof b, &c, "x"),
                "cmake --build 'bd' --target 'test_x' 2>&1"),
        "build: ctest single case");
  c.adapter = AD_PYTEST;
  CHECK(!strcmp(build_cmd(b, sizeof b, &c, NULL), "true"),
        "build: pytest builds nothing by default");
  c.build = "make sim";
  c.configure = "cmake -S . -B bd";
  CHECK(!strcmp(build_cmd(b, sizeof b, &c, NULL),
                "cmake -S . -B bd 2>&1 && make sim 2>&1"),
        "build: build key after configure");
  return 0;
}

/* A runner that dies without reporting a case must fail it, never leave it to
 * read as a pass. A missing test dir makes ctest exit non-zero with no result
 * lines -- the same shape as a crash. */
static int unreported(void) {
  tcase_t cases[1] = {{.name = "t", .status = ST_RUNNING}};
  comp_t c = {.adapter = AD_CTEST, .test_dir = "/nonexistent", .tprefix = "",
              .cases = cases, .ncases = 1};
  int failed = run_suite(&c, NULL, 0);
  CHECK(failed == 1, "unreported: counted as a failure");
  CHECK(cases[0].status == ST_FAIL, "unreported: case marked failed");
  CHECK(cases[0].detail != NULL, "unreported: says why");
  return 0;
}

int main(void) {
  if (ctest_lines() || pytest_lines() || config() || builds() || unreported())
    return 1;
  printf("  %d checks, 0 failures\n", checks);
  return 0;
}
