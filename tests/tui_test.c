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
 * @file tui_test.c
 * @brief The TUI, the key reader, the streaming runner, and every failure path.
 *
 * Same include trick as the other tests, with one addition: the libc calls
 * vtest can see fail (malloc, realloc, popen, pipe, fork, execl, select, read,
 * vsnprintf) are routed through wrappers defined before the include. Each one
 * passes straight through until a test arms it, so the out-of-memory and
 * syscall-failure branches run deterministically instead of never.
 *
 * Keys reach vtest through a pipe on stdin. A suite's command inherits that
 * pipe's write end, so a command can type a key itself (printf q >&N) at
 * exactly the point in a run a test wants it -- no timing, no sleeps.
 *
 * Output goes to stderr: stdout is where vtest draws, and where render() and
 * the batch path print, so it is redirected per test.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* ---------------------------------------------------------- fault injection */
/* Arm with a count N: the Nth call from now fails, the rest pass through. */
static int f_malloc, f_realloc, f_popen, f_pipe, f_fork, f_execl, f_vsnprintf;
static int f_select_eintr, f_select_ebadf, f_read_eintr;
static int f_write_short, f_write_eintr, f_write_err;
static int hit(int *n) { return *n > 0 && --*n == 0; }

/* A canned popen: discovery reads this instead of running ctest/pytest. */
static const char *fake_out;
static FILE *fake_fp;

static void *t_malloc(size_t n) { return hit(&f_malloc) ? NULL : malloc(n); }
static void *t_realloc(void *p, size_t n) {
  return hit(&f_realloc) ? NULL : realloc(p, n);
}
static FILE *t_popen(const char *cmd, const char *mode) {
  if (hit(&f_popen))
    return NULL;
  if (fake_out)
    return fake_fp = fmemopen((void *)fake_out, strlen(fake_out), "r");
  return popen(cmd, mode);
}
static int t_pclose(FILE *fp) {
  if (fp == fake_fp) {
    fake_fp = NULL;
    return fclose(fp);
  }
  return pclose(fp);
}
static int t_pipe(int fd[2]) {
  if (hit(&f_pipe)) {
    errno = EMFILE;
    return -1;
  }
  return pipe(fd);
}
static pid_t t_fork(void) {
  if (hit(&f_fork)) {
    errno = EAGAIN;
    return -1;
  }
  return fork();
}
/* _exit and exec both skip atexit, so a child that leaves through either
 * would drop its gcov counters -- and with them the only coverage of the code
 * that runs there. Dump first. (A weak reference would not do: it does not
 * pull __gcov_dump out of libgcov.a, so it would always read as absent.) */
#ifdef VTEST_COVERAGE
void __gcov_dump(void);
#define gcov_dump() __gcov_dump()
#else
#define gcov_dump() ((void)0)
#endif
static void t_exit(int code) {
  gcov_dump();
  _exit(code);
}

static int t_execl(const char *path, const char *a0, const char *a1,
                   const char *a2, const char *end) {
  if (hit(&f_execl)) {
    errno = ENOENT;
    return -1;
  }
  gcov_dump();
  return execl(path, a0, a1, a2, end);
}
static int t_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) {
  if (hit(&f_select_eintr)) {
    errno = EINTR;
    return -1;
  }
  if (hit(&f_select_ebadf)) {
    errno = EBADF;
    return -1;
  }
  return select(n, r, w, e, tv);
}
static ssize_t t_read(int fd, void *b, size_t n) {
  if (fd == STDIN_FILENO && hit(&f_read_eintr)) {
    errno = EINTR;
    return -1;
  }
  return read(fd, b, n);
}
/* stdout only: short (half), interrupted, or failed. */
static ssize_t t_write(int fd, const void *b, size_t n) {
  if (fd == STDOUT_FILENO && hit(&f_write_short))
    return write(fd, b, n / 2);
  if (fd == STDOUT_FILENO && hit(&f_write_eintr)) {
    errno = EINTR;
    return -1;
  }
  if (fd == STDOUT_FILENO && hit(&f_write_err)) {
    errno = EIO;
    return -1;
  }
  return write(fd, b, n);
}
static int t_vsnprintf(char *s, size_t n, const char *fmt, va_list ap) {
  if (hit(&f_vsnprintf))
    return -1;
  return vsnprintf(s, n, fmt, ap);
}

#define malloc t_malloc
#define realloc t_realloc
#define popen t_popen
#define pclose t_pclose
#define pipe t_pipe
#define fork t_fork
#define execl t_execl
#define select t_select
#define read t_read
#define write t_write
#define vsnprintf t_vsnprintf
#define _exit t_exit
#define main vtest_main
#include "../vtest.c"
#undef main

/* load_config and discovery allocate for the life of a vtest process; this
 * runs them dozens of times. Same reasoning as parse_test.c. */
int __lsan_is_turned_off(void);
int __lsan_is_turned_off(void) { return 1; }

/* ------------------------------------------------------------------ harness */
static int checks = 0;
#define CHECK(c, what)                                                         \
  do {                                                                         \
    checks++;                                                                  \
    if (!(c)) {                                                                \
      fprintf(stderr, "  FAIL %s:%d %s\n", __FILE__, __LINE__, what);          \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int saved_out = -1, saved_in = -1;

/* stdout into a temp file, so what vtest prints can be read back. */
static FILE *cap_fp;
static void cap_begin(void) {
  fflush(stdout);
  cap_fp = tmpfile();
  saved_out = dup(STDOUT_FILENO);
  dup2(fileno(cap_fp), STDOUT_FILENO);
}
static char cap_buf[1 << 16];
static const char *cap_end(void) {
  fflush(stdout);
  dup2(saved_out, STDOUT_FILENO);
  close(saved_out);
  rewind(cap_fp);
  size_t n = fread(cap_buf, 1, sizeof cap_buf - 1, cap_fp);
  cap_buf[n] = 0;
  fclose(cap_fp);
  return cap_buf;
}

/* stdin from a pipe holding `keys`. The write end stays open (and is returned)
 * unless eof is set, so a read after the keys blocks instead of seeing EOF. */
static int keys_in(const char *keys, int eof) {
  int p[2];
  if (pipe(p) != 0)
    return -1;
  ssize_t w = write(p[1], keys, strlen(keys));
  (void)w;
  if (saved_in < 0)
    saved_in = dup(STDIN_FILENO);
  dup2(p[0], STDIN_FILENO);
  close(p[0]);
  if (eof) {
    close(p[1]);
    return -1;
  }
  return p[1];
}
static void keys_done(int wfd) {
  if (wfd >= 0)
    close(wfd);
  dup2(saved_in, STDIN_FILENO);
}

/* ---------------------------------------------------------------- the model */
static tcase_t ct_cases[2], py_cases[2], ck_cases[1];
static comp_t model[3];

/* A known catalog: ctest (pass + multi-line failure), pytest (skip + running),
 * check (pending, no cmd, a note). Reset before every test that reads it. */
static void model_reset(void) {
  memset(ct_cases, 0, sizeof ct_cases);
  memset(py_cases, 0, sizeof py_cases);
  memset(ck_cases, 0, sizeof ck_cases);
  snprintf(ct_cases[0].name, VT_NAME, "ok");
  ct_cases[0].status = ST_PASS;
  ct_cases[0].time_ms = 1.5f;
  snprintf(ct_cases[1].name, VT_NAME, "bad");
  ct_cases[1].status = ST_FAIL;
  ct_cases[1].detail = NULL;
  for (int i = 0; i < 9; i++) {
    char l[32];
    snprintf(l, sizeof l, "fail line %d", i);
    detail_append(&ct_cases[1].detail, l);
  }
  snprintf(py_cases[0].name, VT_NAME, "t.py::skip");
  py_cases[0].status = ST_SKIP;
  snprintf(py_cases[1].name, VT_NAME, "t.py::run");
  py_cases[1].status = ST_RUNNING;
  snprintf(ck_cases[0].name, VT_NAME, "lint");
  memset(model, 0, sizeof model);
  model[0] = (comp_t){.name = "unit", .kind = "ctest", .adapter = AD_CTEST,
                      .test_dir = "build", .filter = "^t", .tprefix = "",
                      .cases = ct_cases, .ncases = 2, .expanded = 1};
  model[1] = (comp_t){.name = "sdk", .kind = "pytest", .adapter = AD_PYTEST,
                      .test_dir = "sdk", .filter = "tests", .tprefix = "",
                      .cases = py_cases, .ncases = 2, .expanded = 1};
  model[2] = (comp_t){.name = "lint", .kind = "check", .adapter = AD_CHECK,
                      .test_dir = ".", .tprefix = "", .cases = ck_cases,
                      .ncases = 1, .expanded = 0};
  snprintf(model[2].note, sizeof model[2].note, "no cmd = in vtest.conf");
  comps = model;
  NCOMPS = 3;
  g_sel = g_top = g_filter = 0;
  g_pane = PANE_LIST;
  g_detail_off = g_log_off = 0;
  g_detail_h = 3;
  g_log_h = 6;
  g_rows = 40;
  g_cols = 100;
  g_status = NULL;
}

/* --------------------------------------------------------------------- tests */
static int statuses(void) {
  model_reset();
  int p, r;
  CHECK(comp_status(&model[0], &p, &r) == ST_FAIL && p == 1 && r == 2,
        "a failure makes the suite FAIL");
  CHECK(comp_status(&model[1], &p, &r) == ST_PENDING && r == 1,
        "a case still running leaves it pending");
  ck_cases[0].status = ST_PASS;
  CHECK(comp_status(&model[2], &p, &r) == ST_PASS, "all ran, none failed");
  comp_t empty = {0};
  CHECK(comp_status(&empty, &p, &r) == ST_PENDING, "no cases is not a pass");

  status_t all[] = {ST_PENDING, ST_RUNNING, ST_PASS, ST_FAIL, ST_SKIP};
  const char *lbl[] = {"----", "----", "PASS", "FAIL", "SKIP"};
  for (int c = 0; c < 2; c++) {
    use_color = c;
    for (int i = 0; i < 5; i++) {
      CHECK(!strcmp(status_label(all[i]), lbl[i]), "status label");
      CHECK(*glyph(all[i]), "every status has a glyph");
      CHECK(!!*glyph_color(all[i]) == c, "colour only when enabled");
    }
  }
  use_color = 0;
  return 0;
}

static int tree_rows(void) {
  model_reset();
  row_t rw[16];
  CHECK(build_rows(rw, 16, 0) == 7, "headers + expanded cases");
  CHECK(rw[1].c == 0 && rw[1].cidx == 0 && rw[6].cidx == -1, "tree order");
  CHECK(build_rows(rw, 16, 1) == 4, "only-failed keeps headers + failures");
  CHECK(build_rows(rw, 2, 0) == 2, "stops at maxrows inside a suite");
  CHECK(build_rows(rw, 1, 0) == 1, "stops at maxrows between suites");
  return 0;
}

static int layouts(void) {
  model_reset();
  int lh, dn, ln;
  layout(&lh, &dn, &ln);
  CHECK(dn == 3 && ln == 6 && lh == 40 - 4 - 4 - 7, "the default split");
  g_rows = 12;
  layout(&lh, &dn, &ln);
  CHECK(lh == 3 && ln == 1 && dn == 2, "short terminal: log gives way first");
  g_rows = 10;
  g_detail_h = 50;
  layout(&lh, &dn, &ln);
  CHECK(lh == 2 && dn == 1 && ln == 1, "an oversized request gives way too");
  g_rows = 5; /* below query_winsize's floor: layout still keeps one row */
  layout(&lh, &dn, &ln);
  CHECK(lh == 1 && dn == 1 && ln == 1, "never below one row each");
  CHECK(list_height() == 1, "list_height agrees");
  return 0;
}

static int nav(void) {
  model_reset();
  row_t rw[16];
  int n = build_rows(rw, 16, 0);

  CHECK(nav_key('x', rw, n) == 0, "not a navigation key");
  CHECK(nav_key('k', rw, n) && g_sel == 0, "up clamps at the first row");
  g_detail_off = 2;
  CHECK(nav_key('j', rw, n) && g_sel == 1 && g_detail_off == 0,
        "down moves, and the new selection's detail starts at the top");
  g_sel = n - 1;
  g_detail_off = 2;
  nav_key('j', rw, n);
  CHECK(g_sel == n - 1 && g_detail_off == 2, "down clamps; detail kept");
  nav_key('j', rw, 0);
  CHECK(g_sel == 0, "an empty tree selects row 0");
  nav_key(KEY_PGDN, rw, n);
  CHECK(g_sel == n - 1, "page down clamps at the end");
  nav_key(KEY_PGUP, rw, n);
  CHECK(g_sel == 0, "page up clamps at the start");
  nav_key(KEY_PGDN, rw, 0);
  CHECK(g_sel == 0, "page down on an empty tree");

  CHECK(nav_key('h', rw, n) && g_pane == PANE_LOG, "left wraps to the log");
  CHECK(nav_key('l', rw, n) && g_pane == PANE_LIST, "right wraps back");

  g_pane = PANE_DETAIL;
  g_dl_n = 20;
  nav_key('j', rw, n);
  CHECK(g_detail_off == 1, "down scrolls the detail pane");
  nav_key(KEY_PGDN, rw, n);
  CHECK(g_detail_off == 3, "page down pages it");
  nav_key(KEY_PGUP, rw, n);
  CHECK(g_detail_off == 1, "page up pages it back");

  g_pane = PANE_LOG;
  g_log_n = 0;
  for (int i = 0; i < 30; i++)
    log_addf("l%d", i);
  nav_key('k', rw, n);
  CHECK(g_log_off == 1, "up scrolls the log back");
  nav_key(KEY_PGUP, rw, n);
  CHECK(g_log_off == 6, "page up pages it back");
  nav_key(KEY_PGDN, rw, n);
  CHECK(g_log_off == 1, "page down pages it forward");
  return 0;
}

static int resize_keys(void) {
  model_reset();
  row_t rw[16];
  int n = build_rows(rw, 16, 0), lh, dn, ln;

  g_pane = PANE_DETAIL;
  nav_key('+', rw, n);
  nav_key('=', rw, n);
  CHECK(g_detail_h == 5, "+ and = grow the focused pane");
  nav_key('-', rw, n);
  CHECK(g_detail_h == 4, "- shrinks it");
  for (int i = 0; i < 10; i++)
    nav_key('-', rw, n);
  CHECK(g_detail_h == 1, "never below one line");

  g_pane = PANE_LOG;
  for (int i = 0; i < 100; i++)
    nav_key('+', rw, n);
  layout(&lh, &dn, &ln);
  CHECK(lh == 3 && ln == g_log_h, "grows until the list is down to three rows");

  g_pane = PANE_LIST;
  g_log_h = 2;
  g_detail_h = 3;
  nav_key('+', rw, n);
  CHECK(g_log_h == 1 && g_detail_h == 3, "growing the list takes from the log");
  nav_key('+', rw, n);
  CHECK(g_log_h == 1 && g_detail_h == 2, "then from detail");
  nav_key('-', rw, n);
  CHECK(g_log_h == 2, "shrinking the list gives the line to the log");

  /* On a short terminal layout() has already squeezed the panes; the keys act
   * on what is on screen, not on a request that no longer fits. */
  g_rows = 12;
  g_pane = PANE_LOG;
  g_log_h = 6;
  g_detail_h = 3;
  nav_key('-', rw, n);
  CHECK(g_log_h == 1 && g_detail_h == 2,
        "shrinking a squeezed pane starts from its visible height");
  return 0;
}

static int frames(void) {
  model_reset();
  use_color = 1;
  sb_t s;
  sb_init(&s);
  row_t rw[16];
  int n = build_rows(rw, 16, 0);

  /* ctest header selected: its run line carries the -R filter. */
  draw_frame(&s, rw, n, 0, 0);
  CHECK(strstr(s.buf, "ctest --test-dir build") && strstr(s.buf, " -R "),
        "ctest header detail");
  CHECK(strstr(s.buf, "✔1") && strstr(s.buf, "✘1"), "header counts");

  /* the failing case: more detail than fits, so the label says where you are */
  s.len = 0;
  g_pane = PANE_DETAIL;
  draw_frame(&s, rw, n, 2, 0);
  CHECK(strstr(s.buf, "detail  1-3 of 10"), "detail position label");
  CHECK(strstr(s.buf, "fail line 0"), "captured output shown");
  g_detail_off = 9;
  s.len = 0;
  draw_frame(&s, rw, n, 2, 0);
  CHECK(strstr(s.buf, "detail  8-10 of 10"), "re-clamped to the last page");

  /* pytest header, a case with no detail, check header with its note */
  s.len = 0;
  draw_frame(&s, rw, n, 3, 0);
  CHECK(strstr(s.buf, "pytest sdk/tests"), "pytest header detail");
  s.len = 0;
  draw_frame(&s, rw, n, 4, 0);
  CHECK(strstr(s.buf, "sdk::t.py::skip"), "case without detail");
  s.len = 0;
  draw_frame(&s, rw, n, 6, 0);
  CHECK(strstr(s.buf, "(no cmd)") && strstr(s.buf, "no cmd = in vtest.conf"),
        "check header: no cmd, and the note");
  model[2].cmd = "make lint";
  model[2].note[0] = 0;
  model[0].filter = NULL;
  s.len = 0;
  draw_frame(&s, rw, n, 6, 0);
  CHECK(strstr(s.buf, "make lint"), "check header: its cmd");
  s.len = 0;
  draw_frame(&s, rw, n, 0, 0);
  CHECK(!strstr(s.buf, " -R "), "ctest with no filter");

  /* scrolled list, scrolled log, status + filter in the footer, no selection */
  g_rows = 12;
  g_status = "running…";
  g_filter = 1;
  g_log_n = 0;
  for (int i = 0; i < 20; i++)
    log_addf("log %d", i);
  g_log_off = 4;
  s.len = 0;
  draw_frame(&s, rw, n, -1, 2);
  CHECK(strstr(s.buf, "▲") && strstr(s.buf, "▼"), "scroll markers");
  CHECK(strstr(s.buf, "back of 20"), "log scrolled label");
  CHECK(strstr(s.buf, "running…") && strstr(s.buf, "[on]"), "footer state");
  g_pane = PANE_LOG;
  s.len = 0;
  draw_frame(&s, rw, n, 99, 0);
  CHECK(strstr(s.buf, "[log]"), "footer names the log pane");
  g_pane = PANE_LIST;
  s.len = 0;
  draw_frame(&s, rw, n, 99, 0);
  CHECK(strstr(s.buf, "[tests]"), "footer names the list pane");

  /* more captured lines than the detail buffer holds */
  char *many = NULL;
  for (int i = 0; i < 400; i++)
    detail_append(&many, "x");
  char *keep = ct_cases[1].detail;
  ct_cases[1].detail = many;
  s.len = 0;
  draw_frame(&s, rw, n, 2, 0);
  CHECK(g_dl_n == DETAIL_CAP, "detail stops at DETAIL_CAP lines");
  ct_cases[1].detail = keep;
  free(many);

  /* a frame bigger than the buffer grows it */
  g_cols = 5000;
  s.len = 0;
  draw_frame(&s, rw, n, 0, 0);
  CHECK(s.cap > 8192, "sb grows");
  f_vsnprintf = 1;
  size_t before = s.len;
  sb_putf(&s, "x");
  CHECK(s.len == before, "a formatting error writes nothing");
  sb_free(&s);
  use_color = 0;
  return 0;
}

static int repaints(void) {
  model_reset();
  int devnull = open("/dev/null", O_WRONLY);
  int out = dup(STDOUT_FILENO);
  dup2(devnull, STDOUT_FILENO);
  sb_init(&g_frame);

  NCOMPS = 0;
  g_filter = 1;
  repaint();
  CHECK(g_filter == 0, "only-failed with no rows turns itself off");
  NCOMPS = 3;
  model[1].expanded = 0;
  g_sel = 100;
  g_rows = 12;
  repaint();
  CHECK(g_sel == 4 && g_top == 2, "selection clamps; the list follows it");
  model[0].expanded = 0;
  repaint();
  CHECK(g_sel == 2 && g_top == 0, "a shorter tree pulls the view back");
  model[0].expanded = 1;
  g_sel = 0;
  repaint();
  CHECK(g_top == 0, "and follows it back up");
  g_top = 3;
  g_sel = 1;
  repaint();
  CHECK(g_top == 1, "a selection above the view scrolls it up");
  g_sel = -5;
  repaint();
  CHECK(g_sel == 0, "negative selection clamps");
  NCOMPS = 0;
  g_sel = 0;
  repaint();
  CHECK(g_sel == 0 && g_top == 0, "an empty catalog");
  NCOMPS = 3;

  /* a frame interrupted mid-write is finished, not dropped */
  sb_free(&g_frame);
  sb_init(&g_frame);
  cap_begin();
  repaint();
  size_t whole = strlen(cap_end());
  f_write_short = 1;
  cap_begin();
  repaint();
  CHECK(strlen(cap_end()) == whole, "a short write is continued");
  f_write_eintr = 1;
  cap_begin();
  repaint();
  CHECK(strlen(cap_end()) == whole, "an interrupted write is retried");
  f_write_err = 1;
  cap_begin();
  repaint();
  CHECK(strlen(cap_end()) == 0, "a failed write gives up");

  sb_free(&g_frame);
  dup2(out, STDOUT_FILENO);
  close(out);
  close(devnull);
  return 0;
}

static int batch_render(void) {
  model_reset();
  row_t rw[16];
  int n = build_rows(rw, 16, 0);
  for (int c = 0; c < 2; c++) {
    use_color = c;
    for (int sel = -1; sel < n; sel++) {
      cap_begin();
      render(rw, n, sel, c, 1);
      const char *o = cap_end();
      CHECK(strstr(o, "test orchestrator"), "render header");
      if (sel == 2)
        CHECK(strstr(o, "fail line 5") && !strstr(o, "fail line 6"),
              "render caps a failure at six lines");
    }
  }
  cap_begin();
  render(rw, n, n, 0, 1);
  CHECK(!strstr(cap_end(), "detail"), "render: selection past the end");
  char *keep = ct_cases[1].detail;
  ct_cases[1].detail = NULL;
  cap_begin();
  render(rw, n, 2, 0, 1);
  CHECK(!strstr(cap_end(), "│"), "render: a failure with no output");
  ct_cases[1].detail = strdup("one\ntwo\n");
  cap_begin();
  render(rw, n, 2, 0, 1);
  CHECK(strstr(cap_end(), "two"), "render: a short failure in full");
  free(ct_cases[1].detail);
  ct_cases[1].detail = keep;
  model[0].filter = NULL;
  cap_begin();
  render(rw, n, 0, 0, 1);
  CHECK(!strstr(cap_end(), " -R "), "render: ctest with no filter");
  cap_begin();
  render(rw, n, 0, 0, 0);
  const char *o = cap_end();
  CHECK(!strstr(o, "detail"), "non-interactive render has no detail");
  CHECK(strstr(o, "no cmd = in vtest.conf"), "notes shown");
  use_color = 0;
  return 0;
}

static int keys(void) {
  struct {
    const char *in;
    int want[4];
  } t[] = {
      {"x", {'x'}},
      {"\x1b[A\x1b[B\x1b[C\x1b[D", {'k', 'j', 'l', 'h'}},
      {"\x1b[5~\x1b[6~", {KEY_PGUP, KEY_PGDN}},
      {"\x1b[5X", {'\x1b'}},  /* PgUp without its tilde */
      {"\x1b[Z", {'\x1b'}},   /* an escape vtest has no use for */
      {"\x1bOx", {'\x1b'}},   /* not a CSI sequence */
      {"\x1b[5", {'\x1b'}},   /* cut short before the tilde */
      {"\x1b[", {'\x1b'}},    /* cut short before the final byte */
      {"\x1b", {'\x1b'}},     /* a bare Esc */
  };
  for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
    int w = keys_in(t[i].in, 1);
    for (int k = 0; k < 4 && t[i].want[k]; k++)
      CHECK(read_key() == t[i].want[k], t[i].in);
    keys_done(w);
  }

  int w = keys_in("x", 1);
  g_resized = 1;
  CHECK(read_key() == KEY_RESIZE, "a resize that interrupted nothing");
  g_resized = 0;
  CHECK(read_key() == 'x', "and the key is still there");
  CHECK(read_key() == 'q', "a closed stdin quits");
  keys_done(w);

  /* An empty non-blocking stdin: retried, then given up on. */
  w = keys_in("", 0);
  fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
  CHECK(read_key() == 'q', "EAGAIN is retried, not spun on forever");
  keys_done(w);

  w = keys_in("", 0);
  f_read_eintr = 1;
  CHECK(read_key() == KEY_RESIZE, "an interrupted read is a resize");
  keys_done(w);

  /* A terminal someone put back in VMIN=0 mode reads 0 bytes: that is not a
   * quit, and it is retried (raw_reassert is what normally fixes the mode). */
  int m, sl;
  CHECK(openpty(&m, &sl, NULL, NULL, NULL) == 0, "openpty");
  struct termios tio;
  tcgetattr(sl, &tio);
  tio.c_lflag &= (unsigned)~(ICANON | ECHO);
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  tcsetattr(sl, TCSANOW, &tio);
  if (saved_in < 0)
    saved_in = dup(STDIN_FILENO);
  dup2(sl, STDIN_FILENO);
  CHECK(read_key() == 'q', "a zero-length read on a tty is retried, then q");

  /* raw mode: entered, re-asserted after a child resets it, restored */
  int out = dup(STDOUT_FILENO);
  dup2(sl, STDOUT_FILENO);
  struct winsize ws = {.ws_row = 5, .ws_col = 10};
  ioctl(sl, TIOCSWINSZ, &ws);
  raw_enter();
  CHECK(raw_active && g_rows == 10 && g_cols == 24,
        "raw mode on; a tiny terminal is clamped to the minimum");
  ws.ws_row = 50;
  ws.ws_col = 120;
  ioctl(sl, TIOCSWINSZ, &ws);
  query_winsize();
  CHECK(g_rows == 50 && g_cols == 120, "real size read");
  ws.ws_row = 0;
  ws.ws_col = 0;
  ioctl(sl, TIOCSWINSZ, &ws);
  query_winsize();
  CHECK(g_rows == 24 && g_cols == 80, "a 0x0 terminal reads as 80x24");
  ws.ws_row = 50;
  ws.ws_col = 0;
  ioctl(sl, TIOCSWINSZ, &ws);
  query_winsize();
  CHECK(g_rows == 24 && g_cols == 80, "so does 50x0");
  tcsetattr(STDIN_FILENO, TCSANOW, &tio);
  raw_reassert();
  tcgetattr(STDIN_FILENO, &tio);
  CHECK(tio.c_cc[VMIN] == 1, "reassert puts VMIN back");
  raw_restore();
  CHECK(!raw_active, "restored");
  raw_restore(); /* idempotent */
  raw_reassert(); /* a no-op outside raw mode */
  int dn = open("/dev/null", O_WRONLY);
  dup2(dn, STDOUT_FILENO);
  close(dn);
  query_winsize();
  dup2(out, STDOUT_FILENO);
  close(out);
  CHECK(g_rows == 24 && g_cols == 80, "no terminal: 80x24");

  /* raw mode on a stdin that is not a terminal: reassert has nothing to get */
  keys_done(-1);
  close(m);
  close(sl);
  w = keys_in("", 1);
  raw_active = 1;
  raw_reassert();
  raw_active = 0;
  keys_done(w);

  on_winch(SIGWINCH);
  CHECK(g_resized == 1, "SIGWINCH flags a resize");
  g_resized = 0;
  return 0;
}

/* Ctrl-C while a suite runs takes the suite's process group down with vtest. */
static int fatal_signal(void) {
  pid_t victim = fork();
  if (victim == 0) {
    setpgid(0, 0);
    pause();
    t_exit(0);
  }
  setpgid(victim, victim);
  pid_t pid = fork();
  if (pid == 0) {
    g_child = victim;
    on_fatal_signal(SIGINT);
  }
  int st;
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 1, "vtest exits 1");
  waitpid(victim, &st, 0);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "the suite was killed");

  pid = fork();
  if (pid == 0) {
    g_child = 0;
    on_fatal_signal(SIGTERM);
  }
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 1, "and with no suite running");
  return 0;
}

static int nlines;
static size_t nchars;
static char lines[8][64];
static void collect(void *v, const char *l) {
  (void)v;
  nchars += strlen(l);
  if (nlines < 8)
    snprintf(lines[nlines], sizeof lines[0], "%.63s", l);
  nlines++;
}

static int streams(void) {
  model_reset();
  int devnull = open("/dev/null", O_WRONLY);
  int out = dup(STDOUT_FILENO);
  dup2(devnull, STDOUT_FILENO);
  sb_init(&g_frame);

  nlines = 0;
  CHECK(stream_exec("printf 'a\\r\\nb\\nc'; exit 3", 0, collect, NULL) == 3,
        "exit status comes back");
  CHECK(nlines == 3 && !strcmp(lines[0], "a") && !strcmp(lines[2], "c"),
        "CR dropped, unterminated last line kept");

  nlines = 0;
  nchars = 0;
  stream_exec("head -c 9000 /dev/zero | tr '\\0' x", 0, collect, NULL);
  CHECK(nlines == 2, "an overlong line is split, not overflowed");
  CHECK(nchars == 9000, "and no character is lost at the split");
  nlines = 0;
  nchars = 0;
  stream_exec("head -c 8191 /dev/zero | tr '\\0' x; echo; echo y", 0, collect,
              NULL);
  CHECK(nlines == 2 && nchars == 8192,
        "a line that exactly fills the buffer is not followed by an empty one");

  CHECK(stream_exec("kill -9 $$", 0, collect, NULL) == -1,
        "killed by a signal is -1");

  f_pipe = 1;
  CHECK(stream_exec("true", 0, collect, NULL) == -1, "no pipe");
  f_fork = 1;
  CHECK(stream_exec("true", 0, collect, NULL) == -1, "no fork");
  f_execl = 1;
  CHECK(stream_exec("true", 0, collect, NULL) == 127, "no shell");
  f_execl = 0; /* armed in the parent too, which never calls it */
  int in = dup(STDIN_FILENO);
  close(STDIN_FILENO);
  nlines = 0;
  CHECK(stream_exec("echo hi", 0, collect, NULL) == 0 && nlines == 1,
        "runs with stdin closed");
  dup2(in, STDIN_FILENO);
  close(in);

  f_select_ebadf = 1;
  stream_exec("echo hi", 0, collect, NULL);
  CHECK(1, "a select error ends the stream");

  /* Interactive: keys arrive while it runs. The command types them itself
   * through the inherited pipe, so they land mid-run, deterministically. */
  char cmd[256];
  int w = keys_in("", 0);
  snprintf(cmd, sizeof cmd, "printf 'jx\\033[B' >&%d; sleep 0.3; echo done", w);
  nlines = 0;
  CHECK(stream_exec(cmd, 1, collect, NULL) == 0, "navigation does not abort");
  CHECK(g_sel == 2, "the keys moved the selection mid-run");

  g_resized = 1;
  CHECK(stream_exec("sleep 0.3", 1, collect, NULL) == 0 && !g_resized,
        "a pending resize is picked up on the idle wake");
  g_resized = 1;
  f_select_eintr = 1;
  CHECK(stream_exec("echo x", 1, collect, NULL) == 0, "resize mid-run");
  CHECK(g_resized == 0, "resize handled");
  f_select_eintr = 1;
  CHECK(stream_exec("echo x", 1, collect, NULL) == 0, "a stray signal");

  snprintf(cmd, sizeof cmd, "printf 'x' >&%d; sleep 0.1; echo y", w);
  f_read_eintr = 1;
  CHECK(stream_exec(cmd, 1, collect, NULL) == 0, "resize read mid-run");

  snprintf(cmd, sizeof cmd, "printf q >&%d; sleep 5", w);
  CHECK(stream_exec(cmd, 1, collect, NULL) == -2, "q aborts the run");
  snprintf(cmd, sizeof cmd, "printf '\\003' >&%d; sleep 5", w);
  CHECK(stream_exec(cmd, 1, collect, NULL) == -2, "so does Ctrl-C");
  keys_done(w);

  sb_free(&g_frame);
  dup2(out, STDOUT_FILENO);
  close(out);
  close(devnull);
  return 0;
}

static int runs(void) {
  model_reset();
  int devnull = open("/dev/null", O_WRONLY);
  int out = dup(STDOUT_FILENO);
  dup2(devnull, STDOUT_FILENO);
  sb_init(&g_frame);
  int w = keys_in("", 0);
  char cmd[256];

  /* a pass, then a failing build that runs anyway */
  tcase_t cc[1] = {{.name = "lint"}};
  comp_t c = {.name = "lint", .kind = "check", .adapter = AD_CHECK,
              .test_dir = ".", .tprefix = "", .cmd = "true", .cases = cc,
              .ncases = 1};
  do_run_interactive(&c, NULL);
  CHECK(cc[0].status == ST_PASS && !g_status, "check passes");
  c.build = "false";
  c.cmd = "exit 1";
  do_run_interactive(&c, "lint");
  CHECK(cc[0].status == ST_FAIL, "a failed build still runs; the run fails");
  do_run_interactive(&c, "nope");
  CHECK(1, "an unknown case name");

  /* aborted during the build, then during the run: nothing left spinning */
  snprintf(cmd, sizeof cmd, "printf q >&%d; sleep 5", w);
  c.build = cmd;
  do_run_interactive(&c, NULL);
  CHECK(cc[0].status == ST_PENDING && !g_status, "build abort resets");
  c.build = NULL;
  c.cmd = cmd;
  do_run_interactive(&c, NULL);
  CHECK(cc[0].status == ST_PENDING && !g_status, "run abort resets");

  comp_t none = {.name = "none", .ncases = 0};
  do_run_interactive(&none, NULL);
  CHECK(1, "a suite with no cases does nothing");

  /* a runner that exits non-zero with every case passed */
  tcase_t pc[1] = {{.name = "t.py::a", .status = ST_RUNNING}};
  comp_t py = {.name = "py", .kind = "pytest", .adapter = AD_PYTEST,
               .test_dir = ".", .filter = "x", .tprefix = "", .cases = pc,
               .ncases = 1};
  snprintf(g_python, sizeof g_python, "echo 't.py::a PASSED'; exit 2; :");
  CHECK(run_suite(&py, NULL, 1) == 1, "a crashed runner is a failure");
  snprintf(g_python, sizeof g_python, "python3");

  keys_done(w);
  sb_free(&g_frame);
  dup2(out, STDOUT_FILENO);
  close(out);
  close(devnull);

  /* batch: an undiscovered suite fails the run; a real one is built + run */
  model_reset();
  model[0].ncases = 0;
  model[1].ncases = 0;
  model[2].cmd = "echo ran";
  ck_cases[0].status = ST_PENDING;
  cap_begin();
  int failed = run_all_batch();
  const char *o = cap_end();
  CHECK(failed == 2, "two suites discovered nothing");
  CHECK(strstr(o, "no cases discovered — suite did not run"), "why, default");
  CHECK(strstr(o, "==> running lint") && strstr(o, "ran"), "the check ran");
  model_reset();
  cap_begin();
  CHECK(run_report(0) == 0, "--list never fails");
  cap_end();
  return 0;
}

/* The main loop, fed keys through a pipe. It ends when the keys do; a run
 * started from it sees that EOF and aborts, which is an abort's path too. */
static int loop_with(const char *keys, int eintr_at) {
  int w = keys_in(keys, 1);
  f_read_eintr = eintr_at;
  int rc = run_interactive();
  keys_done(w);
  return rc;
}

static int interactive_loop(void) {
  int devnull = open("/dev/null", O_WRONLY);
  int out = dup(STDOUT_FILENO);
  dup2(devnull, STDOUT_FILENO);

  /* down; collapse unit; only-failed on, a stray key, off; a resize; page
   * down to lint's header and run it */
  model_reset();
  model[2].cmd = "true";
  g_resized = 1;
  CHECK(loop_with("j fzf\x1b[6~r", 3) == 0, "the loop exits cleanly");
  CHECK(model[0].expanded == 0, "space collapsed the selected suite");
  CHECK(ck_cases[0].status == ST_PENDING, "the aborted run left nothing running");

  model_reset();
  loop_with("jr", 0);
  CHECK(ct_cases[0].status == ST_PENDING, "run one case");
  model_reset();
  model[2].cmd = "true";
  loop_with("a", 0);
  CHECK(ck_cases[0].status == ST_PENDING, "run all");
  model_reset();
  CHECK(loop_with("q", 0) == 0, "q quits");
  model_reset();
  NCOMPS = 0;
  CHECK(loop_with(" r", 0) == 0, "space and r on an empty catalog");

  dup2(out, STDOUT_FILENO);
  close(out);
  close(devnull);
  return 0;
}

static int discovery(void) {
  comp_t c = {.name = "u", .adapter = AD_CTEST, .test_dir = "b", .tprefix = ""};

  /* more cases than the first allocation holds */
  static char big[4096];
  big[0] = 0;
  for (int i = 1; i <= 20; i++) {
    char l[64];
    snprintf(l, sizeof l, "  Test #%d: t%d\n", i, i);
    strcat(big, l);
  }
  strcat(big, "Total Tests: 20\n");
  fake_out = big;
  ctest_discover(&c);
  CHECK(c.ncases == 20 && !strcmp(c.cases[19].name, "t20"), "ctest grows");
  c.filter = "^t";
  ctest_discover(&c);
  CHECK(c.ncases == 20, "with a filter");

  fake_out = "Internal ctest changing into directory: b\n"
             "Failed to change working directory to b : No such file\n";
  ctest_discover(&c);
  CHECK(c.ncases == 0 && strstr(c.note, "not built"), "not built");
  fake_out = "Cannot find file\n";
  ctest_discover(&c);
  CHECK(strstr(c.note, "not built"), "not built, other wording");
  fake_out = "No such file or directory\n";
  ctest_discover(&c);
  CHECK(strstr(c.note, "not built"), "not built, third wording");
  fake_out = "Total Tests: 0\n";
  ctest_discover(&c);
  CHECK(!strcmp(c.note, "no tests matched"), "no tests matched");
  f_popen = 1;
  ctest_discover(&c);
  CHECK(strstr(c.note, "ctest not found"), "no ctest");

  comp_t p = {.name = "p", .adapter = AD_PYTEST, .test_dir = ".",
              .filter = "tests", .tprefix = ""};
  big[0] = 0;
  for (int i = 0; i < 20; i++) {
    char l[64];
    snprintf(l, sizeof l, "tests/a.py::t%d\n", i);
    strcat(big, l);
  }
  strcat(big, "  indented::not_a_node\n==::==\n== 20 tests collected ==\n");
  fake_out = big;
  pytest_discover(&p);
  CHECK(p.ncases == 20, "pytest grows; noise lines skipped");
  fake_out = "noise\n/usr/bin/python3: No module named pytest\n";
  pytest_discover(&p);
  CHECK(strstr(p.note, "pytest unavailable"), "no pytest");
  fake_out = "sh: python9: not found\n";
  pytest_discover(&p);
  CHECK(strstr(p.note, "pytest unavailable"), "no python");
  fake_out = "no tests ran\n";
  pytest_discover(&p);
  CHECK(!strcmp(p.note, "no tests collected"), "nothing collected");
  f_popen = 1;
  pytest_discover(&p);
  CHECK(!strcmp(p.note, "python not found"), "popen failed");

  /* an empty suite that can configure itself does, then looks again */
  fake_out = "Total Tests: 0\n";
  c.configure = "true";
  discover_comp(&c);
  CHECK(c.ncases == 0, "configured, still empty");
  c.configure = "false";
  discover_comp(&c);
  CHECK(c.ncases == 0, "configure failed");
  fake_out = "";
  p.configure = "true";
  discover_comp(&p);
  CHECK(p.ncases == 0, "pytest configure");
  comp_t k = {.name = "k", .adapter = AD_CHECK, .cmd = "true"};
  discover_comp(&k);
  CHECK(k.ncases == 1 && !k.note[0], "check: one case, no note");
  fake_out = NULL;

  /* discover_all over a two-suite catalog */
  static comp_t two[2];
  two[0] = two[1] = k;
  two[0].cases = NULL;
  two[1].cases = NULL;
  comps = two;
  NCOMPS = 2;
  discover_all();
  CHECK(two[0].ncases == 1 && two[1].ncases == 1, "discover_all");
  return 0;
}

static int captures(void) {
  char *o = run_capture("seq 1 5000");
  CHECK(o && strstr(o, "\n5000\n"), "a big capture grows its buffer");
  free(o);
  f_malloc = 1;
  CHECK(run_capture("true") == NULL, "no buffer");
  f_realloc = 1;
  o = run_capture("seq 1 5000");
  CHECK(o && strlen(o) <= 8192, "a failed grow keeps what it has");
  free(o);
  f_popen = 1;
  CHECK(run_capture("true") == NULL, "no popen");

  /* detail capture is capped, and survives a failed grow */
  char *d = NULL;
  for (int i = 0; i < 200; i++)
    detail_append(&d, "0123456789");
  CHECK(strlen(d) < 1500, "detail is capped");
  free(d);
  d = NULL;
  f_realloc = 1;
  detail_append(&d, "x");
  CHECK(d == NULL, "detail survives OOM");

  /* xrealloc: out of memory is the end of the run, said once */
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    dup2(dn, STDERR_FILENO);
    f_realloc = 1;
    xrealloc(NULL, 16);
    t_exit(0);
  }
  int st;
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 1, "xrealloc exits 1 on OOM");
  return 0;
}

/* The parsers' edge cases the other file does not reach. */
static int parse_edges(void) {
  char name[8];
  status_t st;
  float ms;
  CHECK(!parse_discovery_line("Test : x #1", name, sizeof name),
        "discovery: colon before hash");
  CHECK(!parse_discovery_line("Test #1 no colon", name, sizeof name),
        "discovery: no colon");
  CHECK(!parse_discovery_line("Test no hash: x", name, sizeof name),
        "discovery: no hash");
  CHECK(!parse_discovery_line("  Test #1:   \r\n", name, sizeof name),
        "discovery: empty name");
  CHECK(parse_discovery_line("Test #1: x \r\n", name, sizeof name) &&
            !strcmp(name, "x"),
        "discovery: trailing space and CR");
  CHECK(parse_discovery_line("Test #1: a_very_long_name\n", name, sizeof name) &&
            !strcmp(name, "a_very_"),
        "discovery: truncated to the buffer");

  CHECK(!parse_result_line("nothing here", name, sizeof name, &st, &ms),
        "result: no Test");
  CHECK(!parse_result_line("Test without hash", name, sizeof name, &st, &ms),
        "result: no hash");
  CHECK(!parse_result_line("Test #1 no colon", name, sizeof name, &st, &ms),
        "result: no colon");
  CHECK(parse_result_line("Test #1: long_name_here ... Passed 0.5 sec", name,
                          sizeof name, &st, &ms) &&
            !strcmp(name, "long_na"),
        "result: truncated to the buffer");
  CHECK(parse_result_line("Test #1: x ... Passed", name, sizeof name, &st, &ms) &&
            ms == 0.0f,
        "result: no time");
  CHECK(parse_result_line("Test #1: a b  ... Passed", name, sizeof name, &st,
                          &ms) &&
            !strcmp(name, "a b"),
        "result: a space in the name, padding before the dots");
  CHECK(parse_result_line("Test #1: x ... Passed sec", name, sizeof name, &st,
                          &ms) &&
            ms == 0.0f,
        "result: sec with no number");
  CHECK(parse_result_line("sec Test #1: x ... Passed", name, sizeof name, &st,
                          &ms),
        "result: sec at the very start");
  CHECK(parse_result_line("Test #1: x ... 1234567890123456789012345678901234 sec",
                          name, sizeof name, &st, &ms) &&
            ms == 0.0f,
        "result: a number too long to be one");

  /* ctest_parse_line: a failure's output is its detail, until the summary */
  tcase_t cs[1] = {{.name = "a"}};
  comp_t c = {.cases = cs, .ncases = 1};
  int failed = 0;
  tcase_t *cur = NULL;
  ctest_parse_line(&c, "orphan line", &failed, &cur);
  ctest_parse_line(&c, "1/1 Test #1: a ...***Failed 0.1 sec", &failed, &cur);
  ctest_parse_line(&c, "boom", &failed, &cur);
  CHECK(failed == 1 && cs[0].detail && strstr(cs[0].detail, "boom"),
        "detail captured");
  ctest_parse_line(&c, "0% tests passed, 1 tests failed", &failed, &cur);
  CHECK(cur == NULL, "summary ends the capture");
  const char *enders[] = {"The following tests FAILED:", "Total Test time"};
  for (int i = 0; i < 2; i++) {
    cur = cs;
    ctest_parse_line(&c, enders[i], &failed, &cur);
    CHECK(cur == NULL, enders[i]);
  }
  ctest_parse_line(&c, "1/1 Test #1: zz ...   Passed 0.1 sec", &failed, &cur);
  CHECK(cur == NULL, "an unknown case is ignored");
  ctest_parse_line(&c, "1/1 Test #1: a ...   Passed 0.1 sec", &failed, &cur);
  CHECK(cs[0].status == ST_PASS && !cs[0].detail, "a rerun clears the detail");

  /* pytest: every tag, an unknown node, an overlong line */
  tcase_t pc[1] = {{.name = "n"}};
  comp_t p = {.cases = pc, .ncases = 1};
  const char *tags[] = {"ERROR", "SKIPPED", "XFAIL", "XPASS"};
  status_t want[] = {ST_FAIL, ST_SKIP, ST_SKIP, ST_PASS};
  for (int i = 0; i < 4; i++) {
    char l[64];
    snprintf(l, sizeof l, "n %s [ 1%%]", tags[i]);
    pytest_parse_line(&p, l, &failed);
    CHECK(pc[0].status == want[i], tags[i]);
  }
  pc[0].status = ST_PENDING;
  pytest_parse_line(&p, "n   PASSED", &failed);
  CHECK(pc[0].status == ST_PASS, "pytest: padding before the tag");
  pytest_parse_line(&p, "other PASSED", &failed);
  pytest_parse_line(&p, " PASSED", &failed);
  char longl[300];
  memset(longl, 'a', 250);
  strcpy(longl + 250, " PASSED");
  pytest_parse_line(&p, longl, &failed);
  CHECK(pc[0].status == ST_PASS, "unknown / empty / overlong nodes ignored");

  /* run commands */
  static char b[16384];
  comp_t r = {.test_dir = "d", .filter = "^f", .name = "g", .tprefix = ""};
  ctest_run_cmd(b, sizeof b, &r, "x");
  CHECK(strstr(b, "-R '^x$'"), "ctest single case");
  ctest_run_cmd(b, sizeof b, &r, NULL);
  CHECK(strstr(b, "-R '^f'"), "ctest filter");
  r.filter = NULL;
  ctest_run_cmd(b, sizeof b, &r, NULL);
  CHECK(!strstr(b, "-R"), "ctest all");
  r.filter = "tests";
  pytest_run_cmd(b, sizeof b, &r, "tests/a.py::t");
  CHECK(strstr(b, "'tests/a.py::t' -v"), "pytest single node");
  pytest_run_cmd(b, sizeof b, &r, NULL);
  CHECK(strstr(b, "'tests' -v"), "pytest filter");
  check_run_cmd(b, sizeof b, &r, NULL);
  CHECK(strstr(b, "( false )"), "a check with no cmd fails");

  comp_t k = {.name = "k", .adapter = AD_CHECK};
  check_discover(&k);
  CHECK(strstr(k.note, "no cmd"), "check with no cmd says so");
  free(k.cases);

  /* config: more suites than the first allocation, every key */
  char path[] = "/tmp/vtest_tui_XXXXXX";
  int fd = mkstemp(path);
  FILE *fp = fdopen(fd, "w");
  for (int i = 0; i < 12; i++)
    fprintf(fp, "[s%d]\n\tconfigure = c\nbuild\t=\tb\ncmd = x\nprefix = p_\n",
            i);
  fclose(fp);
  CHECK(load_config(path) == 12 && !strcmp(comps[11].name, "s11") &&
            !strcmp(comps[11].configure, "c") && !strcmp(comps[0].build, "b") &&
            !strcmp(comps[0].tprefix, "p_"),
        "config grows past eight suites");
  unlink(path);

  /* log ring wraps */
  g_log_n = g_log_head = g_log_off = 0;
  for (int i = 0; i < LOG_CAP + 5; i++)
    log_addf("%d", i);
  log_add(NULL);
  CHECK(g_log_n == LOG_CAP && !strcmp(log_line(LOG_CAP - 1), ""),
        "the ring holds LOG_CAP lines; NULL logs as empty");
  g_log_off = LOG_CAP;
  log_add("x");
  CHECK(g_log_off == LOG_CAP, "a full scroll-back does not overflow");
  g_log_off = 0;
  CHECK(log_line(-1) == NULL && log_line(LOG_CAP) == NULL, "out of range");

  /* batch-mode build lines echo to stdout */
  int zero = 0;
  cap_begin();
  on_build_line(&zero, "built");
  CHECK(strstr(cap_end(), "built"), "batch echo");
  cap_begin();
  rule();
  CHECK(strstr(cap_end(), "─"), "rule");
  return 0;
}

int main(void) {
  if (statuses() || tree_rows() || layouts() || nav() || resize_keys() || frames() ||
      repaints() || batch_render() || keys() || fatal_signal() || streams() ||
      runs() || interactive_loop() || discovery() || captures() ||
      parse_edges())
    return 1;
  fprintf(stderr, "  %d checks, 0 failures\n", checks);
  return 0;
}
