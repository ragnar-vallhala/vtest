/* vtest.c — Vayu test orchestrator (interactive TUI) — REAL ctest adapter.
 *
 * Repo-root, cross-component orchestrator. This build drives the real ctest
 * suites that `tools/scripts/vayu.sh test` runs:
 *     sitl  ->  ctest --test-dir build_sitl
 *     gcs   ->  ctest --test-dir navigator/build -R ^tst_
 * It discovers cases with `ctest -N`, runs them with `ctest ... --output-on-
 * failure`, and parses the result lines — no XML/JSON, no deps. (pytest +
 * native-C adapters are the next step; only ctest is wired here.)
 *
 * Build:  gcc -std=c11 -Wall -Wextra vtest/vtest.c -o /tmp/vtest
 * Run:    /tmp/vtest          interactive TUI (run from the repo root)
 *         /tmp/vtest --run     non-interactive: discover + run all + report
 *         /tmp/vtest --list    discover + print the catalog, don't run
 *
 * Zero deps: raw ANSI + termios, no ncurses. Color auto-off when not a TTY.
 */
#define _POSIX_C_SOURCE                                                        \
  200809L /* popen/pclose, strtok_r, strdup under -std=c11 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* realloc() into the same pointer leaks the original and then dereferences
 * NULL. This is a terminal TUI; out of memory is the end of the run either
 * way, so say so once here instead of at three call sites. */
static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q) {
    free(p);
    fputs("vtest: out of memory\n", stderr);
    exit(1);
  }
  return q;
}

#define VT_W 78
#define VT_NAME 128

typedef enum { ST_PENDING, ST_RUNNING, ST_PASS, ST_FAIL, ST_SKIP } status_t;

typedef struct {
  char name[VT_NAME];
  status_t status;
  float time_ms;
  char *detail; /* captured failure output (heap), or NULL */
} tcase_t;

typedef enum { AD_CTEST, AD_PYTEST } adapter_t;

typedef struct {
  const char *name;     /* component label */
  const char *kind;     /* display tag: "ctest" / "pytest" */
  adapter_t adapter;    /* which runner adapter drives this component */
  const char *test_dir; /* ctest: cmake binary dir;  pytest: pytest rootdir */
  const char
      *filter; /* ctest: -R <regex> (NULL=all);  pytest: subdir ("tests") */
  const char *tprefix; /* ctest: test name -> build target = tprefix + name */
  tcase_t *cases;
  int ncases;
  int expanded;
  char note[160]; /* e.g. "not built — run: vayu.sh build sitl" */
} comp_t;

/* The catalog: every test suite in the monorepo, by runner.
 *   ctest  — tprefix maps a test name to its CMake target so it builds alone
 *            (sim/host: fs_owner -> test_fs_owner; navigator: tst_x -> tst_x).
 *   pytest — test_dir is the rootdir; filter is the subdir to collect/run. */
static comp_t comps[] = {
    {"sitl", "ctest", AD_CTEST, "build_sitl", NULL, "test_", NULL, 0, 1, ""},
    {"gcs", "ctest", AD_CTEST, "navigator/build", "^tst_", "", NULL, 0, 0, ""},
    {"fw-host", "ctest", AD_CTEST, "build_fwtest", NULL, "test_", NULL, 0, 0,
     ""},
    {"headless", "pytest", AD_PYTEST, "navigator/headless-sdk", "tests", "",
     NULL, 0, 0, ""},
};
static const int NCOMPS = (int)(sizeof comps / sizeof comps[0]);

static char g_cwd[4096] = "."; /* repo root, absolute */
static char g_python[4128] =
    "python3"; /* pytest interpreter (.venv if present) */

/* defined later (they need the TUI repaint); declared here for the batch path. */
static void repaint(void);
static int build_suite(comp_t *c, const char *only, int interactive);
static int run_suite(comp_t *c, const char *only, int interactive);

/* ============================================================ ctest adapter */

/* Run `cmd` (with 2>&1) and return its full stdout as a heap string (caller
 * frees), or NULL on spawn failure. */
static char *run_capture(const char *cmd) {
  /* NOLINTNEXTLINE(cert-env33-c): shelling out to ctest/pytest is the job. */
  FILE *fp = popen(cmd, "r");
  if (!fp)
    return NULL;
  size_t cap = 8192, len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    pclose(fp);
    return NULL;
  }
  size_t n;
  char tmp[2048];
  while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) {
    if (len + n + 1 > cap) {
      while (len + n + 1 > cap)
        cap *= 2;
      char *nb = realloc(buf, cap);
      if (!nb)
        break;
      buf = nb;
    }
    memcpy(buf + len, tmp, n);
    len += n;
  }
  buf[len] = 0;
  pclose(fp);
  return buf;
}

/* "  Test  #1: safety_phase2"  ->  name="safety_phase2". Returns 1 on match. */
static int parse_discovery_line(const char *line, char *name, size_t namesz) {
  const char *p = strstr(line, "Test ");
  if (!p)
    return 0;
  const char *hash = strchr(p, '#');
  const char *colon = strchr(p, ':');
  if (!hash || !colon || hash > colon)
    return 0;
  const char *q = colon + 1;
  while (*q == ' ')
    q++;
  size_t len = strlen(q);
  while (len && (q[len - 1] == '\n' || q[len - 1] == '\r' || q[len - 1] == ' '))
    len--;
  if (!len)
    return 0;
  if (len >= namesz)
    len = namesz - 1;
  memcpy(name, q, len);
  name[len] = 0;
  return 1;
}

/* "1/15 Test #1: foo ....   Passed    0.12 sec" -> name/status/ms. The trailing
 * dots + a status word distinguish a result line from a bare discovery line. */
static int parse_result_line(const char *line, char *name, size_t namesz,
                             status_t *st, float *ms) {
  /* ctest pads single-digit ids: "Test  #1" (two spaces) vs "Test #10" — so
   * locate "Test", then '#', then ':' separately rather than a fixed "Test #". */
  const char *p = strstr(line, "Test ");
  if (!p)
    return 0;
  const char *hash = strchr(p, '#');
  const char *colon = hash ? strchr(hash, ':') : NULL;
  if (!hash || !colon)
    return 0;
  p = colon + 1;
  while (*p == ' ')
    p++;
  /* name runs up to " ." (space then the leader dots) */
  const char *e = p;
  while (*e && !(e[0] == ' ' && e[1] == '.'))
    e++;
  if (!*e)
    return 0; /* no dots -> not a result line */
  size_t len = (size_t)(e - p);
  while (len && p[len - 1] == ' ')
    len--;
  if (len >= namesz)
    len = namesz - 1;
  memcpy(name, p, len);
  name[len] = 0;

  if (strstr(line, "Passed"))
    *st = ST_PASS;
  else if (strstr(line, "Skipped"))
    *st = ST_SKIP;
  else
    *st = ST_FAIL; /* "Failed", and anything unrecognised, is a failure */

  *ms = 0.0f;
  const char *s = strstr(line, "sec");
  if (s) {
    const char *q = s;
    while (q > line && q[-1] == ' ')
      q--;
    const char *numend = q;
    while (q > line && (isdigit((unsigned char)q[-1]) || q[-1] == '.'))
      q--;
    size_t nl = (size_t)(numend - q);
    char num[32];
    if (nl && nl < sizeof num) {
      memcpy(num, q, nl);
      num[nl] = 0;
      *ms = (float)strtod(num, NULL) * 1000.0f;
    }
  }
  return 1;
}

static tcase_t *find_case(comp_t *c, const char *name) {
  for (int i = 0; i < c->ncases; i++)
    if (strcmp(c->cases[i].name, name) == 0)
      return &c->cases[i];
  return NULL;
}

/* Append one line (cap the buffer so a chatty failure can't run away). */
static void detail_append(char **d, const char *line) {
  size_t cur = *d ? strlen(*d) : 0;
  if (cur > 1400)
    return;
  size_t add = strlen(line) + 2;
  char *nb = realloc(*d, cur + add + 1);
  if (!nb)
    return;
  *d = nb;
  memcpy(*d + cur, line, add - 2);
  (*d)[cur + add - 2] = '\n';
  (*d)[cur + add - 1] = 0;
}

/* Discover a component's cases via `ctest -N`. Sets comp->note on failure. */
static void ctest_discover(comp_t *c) {
  char cmd[512];
  if (c->filter)
    snprintf(cmd, sizeof cmd, "ctest --test-dir '%s' -N -R '%s' 2>&1",
             c->test_dir, c->filter);
  else
    snprintf(cmd, sizeof cmd, "ctest --test-dir '%s' -N 2>&1", c->test_dir);

  char *out = run_capture(cmd);
  c->note[0] = 0;
  free(c->cases);
  c->cases = NULL;
  c->ncases = 0;
  if (!out) {
    snprintf(c->note, sizeof c->note, "ctest not found on PATH");
    return;
  }
  int cap = 8, n = 0;
  tcase_t *arr = malloc((size_t)cap * sizeof *arr);
  char *save = NULL;
  for (char *line = strtok_r(out, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char name[VT_NAME];
    if (parse_discovery_line(line, name, sizeof name)) {
      if (n == cap) {
        cap *= 2;
        arr = xrealloc(arr, (size_t)cap * sizeof *arr);
      }
      memset(&arr[n], 0, sizeof arr[n]);
      snprintf(arr[n].name, VT_NAME, "%s", name);
      arr[n].status = ST_PENDING;
      n++;
    }
  }
  if (n == 0) {
    if (strstr(out, "Failed to change working directory") ||
        strstr(out, "Cannot find") || strstr(out, "No such file"))
      snprintf(c->note, sizeof c->note, "not built — run: vayu.sh build %s",
               c->name);
    else
      snprintf(c->note, sizeof c->note, "no tests matched");
    free(arr);
  } else {
    c->cases = arr;
    c->ncases = n;
  }
  free(out);
}

/* The ctest run command for a component (only==NULL) or a single case.
 * stdbuf -oL forces line-buffered stdout so the pipe streams per test. */
static void ctest_run_cmd(char *buf, size_t n, comp_t *c, const char *only) {
  char rflag[256] = "";
  if (only)
    snprintf(rflag, sizeof rflag, " -R '^%s$'", only);
  else if (c->filter)
    snprintf(rflag, sizeof rflag, " -R '%s'", c->filter);
  snprintf(buf, n,
           "stdbuf -oL -eL ctest --test-dir '%s'%s --output-on-failure 2>&1",
           c->test_dir, rflag);
}

/* Parse one line of ctest output into the model. `cur` carries the failing
 * case across calls so the output block after a ✘ result line is captured as
 * its detail. Increments *failed on a new failure. */
static void ctest_parse_line(comp_t *c, const char *line, int *failed,
                             tcase_t **cur) {
  char name[VT_NAME];
  status_t st;
  float ms;
  if (parse_result_line(line, name, sizeof name, &st, &ms)) {
    tcase_t *tc = find_case(c, name);
    if (tc) {
      tc->status = st;
      tc->time_ms = ms;
      free(tc->detail);
      tc->detail = NULL;
      if (st == ST_FAIL)
        (*failed)++;
    }
    *cur = (tc && st == ST_FAIL) ? tc : NULL;
  } else if (*cur) {
    if (strstr(line, "tests passed") ||
        strstr(line, "The following tests FAILED") ||
        strstr(line, "Total Test time"))
      *cur = NULL;
    else
      detail_append(&(*cur)->detail, line);
  }
}

/* ----------------------------------------------------------- pytest adapter */
/* Discover via `pytest --collect-only -q`, run from the rootdir so nodeids are
 * rootdir-relative (tests/...) in both discovery and the -v run output. */
static void pytest_discover(comp_t *c) {
  char cmd[8192];
  snprintf(cmd, sizeof cmd,
           "cd '%s' && %s -m pytest '%s' --collect-only -q 2>&1", c->test_dir,
           g_python, c->filter);
  char *out = run_capture(cmd);
  c->note[0] = 0;
  free(c->cases);
  c->cases = NULL;
  c->ncases = 0;
  if (!out) {
    snprintf(c->note, sizeof c->note, "python not found");
    return;
  }
  int cap = 16, n = 0;
  tcase_t *arr = malloc((size_t)cap * sizeof *arr);
  char *save = NULL;
  for (char *l = strtok_r(out, "\n", &save); l;
       l = strtok_r(NULL, "\n", &save)) {
    /* a collected nodeid line: "tests/...::test_x", not indented, has "::" */
    if (strstr(l, "::") && l[0] && l[0] != ' ' && l[0] != '=') {
      if (n == cap) {
        cap *= 2;
        arr = xrealloc(arr, (size_t)cap * sizeof *arr);
      }
      memset(&arr[n], 0, sizeof arr[n]);
      snprintf(arr[n].name, VT_NAME, "%s", l);
      arr[n].status = ST_PENDING;
      n++;
    }
  }
  if (n == 0) {
    if (strstr(out, "No module named pytest") || strstr(out, "not found"))
      snprintf(c->note, sizeof c->note,
               "pytest unavailable (.venv) — run: vayu.sh test headless");
    else
      snprintf(c->note, sizeof c->note, "no tests collected");
    free(arr);
  } else {
    c->cases = arr;
    c->ncases = n;
  }
  free(out);
}

/* The pytest run command: run from the rootdir (nodeids stay rootdir-relative,
 * matching discovery). PYTHONUNBUFFERED + stdbuf -oL so the pipe streams per
 * test. Integration tests read VAYU_SITL_RTOS_BIN (the single in-process SITL
 * binary that replaced the vsim_d + vayu_sitl pair). */
static void pytest_run_cmd(char *buf, size_t n, comp_t *c, const char *only) {
  snprintf(buf, n,
           "cd '%s' && PYTHONUNBUFFERED=1 "
           "VAYU_SITL_RTOS_BIN='%s/build_sitl_rtos/vayu_sitl_rtos' "
           "stdbuf -oL -eL %s -m pytest '%s' -v 2>&1",
           c->test_dir, g_cwd, g_python, only ? only : c->filter);
}

/* Parse one line of pytest -v output: "<nodeid> PASSED [..%]". */
static void pytest_parse_line(comp_t *c, const char *l, int *failed) {
  static const struct {
    const char *tag;
    status_t st;
  } kTags[] = {
      {" PASSED", ST_PASS},  {" FAILED", ST_FAIL}, {" ERROR", ST_FAIL},
      {" SKIPPED", ST_SKIP}, {" XFAIL", ST_SKIP},
  };
  const char *p = NULL;
  status_t s = ST_FAIL;
  for (size_t i = 0; i < sizeof kTags / sizeof *kTags && !p; i++) {
    p = strstr(l, kTags[i].tag);
    s = kTags[i].st;
  }
  if (!p)
    return;
  size_t L = (size_t)(p - l);
  while (L && l[L - 1] == ' ')
    L--;
  char name[VT_NAME];
  if (L >= VT_NAME)
    L = VT_NAME - 1;
  memcpy(name, l, L);
  name[L] = 0;
  tcase_t *tc = find_case(c, name);
  if (tc) {
    tc->status = s;
    if (s == ST_FAIL)
      (*failed)++;
  }
}

/* ----------------------------------------------------------- adapter dispatch */
static void discover_comp(comp_t *c) {
  if (c->adapter == AD_PYTEST)
    pytest_discover(c);
  else
    ctest_discover(c);
}

/* ============================================================== terminal/ui */
static int use_color = 0;
static struct termios saved_tio;
static int raw_active = 0;
static int g_rows = 24, g_cols = 80;        /* current terminal size */
static volatile sig_atomic_t g_resized = 0; /* SIGWINCH flag */

static const char *C(const char *code) { return use_color ? code : ""; }
#define CRESET C("\x1b[0m")
#define CBOLD C("\x1b[1m")
#define CDIM C("\x1b[2m")
#define CGREEN C("\x1b[32m")
#define CRED C("\x1b[31m")
#define CYEL C("\x1b[33m")
#define CCYAN C("\x1b[36m")

/* growable byte buffer — a frame is built here and written in ONE syscall, so
 * the terminal never shows a half-drawn screen (no flicker). */
typedef struct {
  char *buf;
  size_t len, cap;
} sb_t;
static void sb_init(sb_t *s) {
  s->cap = 8192;
  s->len = 0;
  s->buf = malloc(s->cap);
  s->buf[0] = 0;
}
static void sb_free(sb_t *s) {
  free(s->buf);
  s->buf = NULL;
}
static void sb_putf(sb_t *s, const char *fmt, ...) {
  for (;;) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n < 0)
      return;
    if ((size_t)n < s->cap - s->len) {
      s->len += (size_t)n;
      return;
    }
    s->cap *= 2;
    s->buf = xrealloc(s->buf, s->cap); /* grow + retry */
  }
}

static void query_winsize(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
    g_rows = ws.ws_row;
    g_cols = ws.ws_col;
  } else {
    g_rows = 24;
    g_cols = 80;
  }
  if (g_rows < 10)
    g_rows = 10;
  if (g_cols < 24)
    g_cols = 24;
}

/* ---- UI state shared with the streaming-build repaint path ---------------- */
static int g_sel = 0, g_top = 0, g_filter = 0;
static const char *g_status =
    NULL;            /* transient footer status (building/running) */
static sb_t g_frame; /* reused frame buffer */

/* Log ring — build + run output shown live in the log pane. */
#define LOG_CAP 600
static char *g_log[LOG_CAP];
static int g_log_n = 0, g_log_head = 0;
static void log_add(const char *line) {
  free(g_log[g_log_head]);
  g_log[g_log_head] = strdup(line ? line : "");
  g_log_head = (g_log_head + 1) % LOG_CAP;
  if (g_log_n < LOG_CAP)
    g_log_n++;
}
static void log_addf(const char *fmt, ...) {
  char b[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  log_add(b);
}
/* a in [0, g_log_n): line index oldest(0)..newest(g_log_n-1). */
static const char *log_line(int a) {
  if (a < 0 || a >= g_log_n)
    return NULL;
  int idx = ((g_log_head - g_log_n + a) % LOG_CAP + LOG_CAP) % LOG_CAP;
  return g_log[idx];
}

/* The on-demand build command for a component (only != NULL = single ctest
 * target). ctest builds its CMake target(s); pytest builds the single
 * in-process SITL binary its integration tests drive. */
static const char *build_cmd(char *buf, size_t n, comp_t *c, const char *only) {
  if (c->adapter == AD_PYTEST)
    snprintf(buf, n,
             "cmake --build build_sitl_rtos --target vayu_sitl_rtos 2>&1");
  else if (only)
    snprintf(buf, n, "cmake --build '%s' --target '%s%s' 2>&1", c->test_dir,
             c->tprefix, only);
  else
    snprintf(buf, n, "cmake --build '%s' 2>&1", c->test_dir);
  return buf;
}

static void raw_restore(void) {
  if (raw_active) {
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
    /* re-enable autowrap, show cursor, leave the alternate screen -> the user's
     * shell scrollback comes back exactly as it was before vtest launched. */
    const char *leave = "\x1b[?7h\x1b[?25h\x1b[?1049l";
    ssize_t w = write(STDOUT_FILENO, leave, strlen(leave));
    (void)w;
    raw_active = 0;
  }
}
static void on_fatal_signal(int sig) {
  (void)sig;
  raw_restore();
  _exit(1);
}
static void on_winch(int sig) {
  (void)sig;
  g_resized = 1;
}

static void raw_enter(void) {
  tcgetattr(STDIN_FILENO, &saved_tio);
  struct termios t = saved_tio;
  t.c_lflag &= (unsigned)~(ICANON | ECHO);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &t);
  raw_active = 1;
  atexit(raw_restore);
  /* enter alternate screen, hide cursor, disable autowrap: vtest now owns the
   * whole viewport and overlong lines clip at the margin instead of wrapping. */
  const char *enter = "\x1b[?1049h\x1b[?25l\x1b[?7l";
  ssize_t w = write(STDOUT_FILENO, enter, strlen(enter));
  (void)w;
  signal(SIGINT, on_fatal_signal);
  signal(SIGTERM, on_fatal_signal);
  /* SIGWINCH without SA_RESTART so a resize interrupts the blocking read(). */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_winch;
  sigaction(SIGWINCH, &sa, NULL);
  query_winsize();
}

static const char *glyph(status_t s) {
  switch (s) {
  case ST_PASS:
    return "✔";
  case ST_FAIL:
    return "✘";
  case ST_RUNNING:
    return "…";
  case ST_SKIP:
    return "○";
  default:
    return "·";
  }
}
static const char *glyph_color(status_t s) {
  switch (s) {
  case ST_PASS:
    return CGREEN;
  case ST_FAIL:
    return CRED;
  case ST_RUNNING:
  case ST_SKIP:
    return CYEL;
  default:
    return CDIM;
  }
}
static const char *status_label(status_t s) {
  switch (s) {
  case ST_PASS:
    return "PASS";
  case ST_FAIL:
    return "FAIL";
  case ST_SKIP:
    return "SKIP";
  default:
    return "----";
  }
}

static status_t comp_status(const comp_t *c, int *passed, int *ran) {
  int p = 0, r = 0, fail = 0;
  for (int i = 0; i < c->ncases; i++) {
    status_t st = c->cases[i].status;
    if (st == ST_PASS) {
      p++;
      r++;
    } else if (st == ST_FAIL) {
      fail++;
      r++;
    } else if (st == ST_SKIP) {
      r++;
    }
  }
  *passed = p;
  *ran = r;
  if (fail)
    return ST_FAIL;
  if (r == c->ncases && c->ncases > 0)
    return ST_PASS;
  return ST_PENDING;
}

typedef struct {
  int c, cidx;
} row_t;
static int build_rows(row_t *rows, int maxrows, int filter_failed) {
  int n = 0;
  for (int ci = 0; ci < NCOMPS && n < maxrows; ci++) {
    rows[n].c = ci;
    rows[n].cidx = -1;
    n++;
    if (comps[ci].expanded)
      for (int k = 0; k < comps[ci].ncases && n < maxrows; k++) {
        if (filter_failed && comps[ci].cases[k].status != ST_FAIL)
          continue;
        rows[n].c = ci;
        rows[n].cidx = k;
        n++;
      }
  }
  return n;
}

static void rule(void) {
  printf(" %s", CDIM);
  for (int i = 0; i < VT_W; i++)
    printf("─");
  printf("%s\n", CRESET);
}

static void render(const row_t *rows, int nrows, int sel, int filter_failed,
                   int interactive) {
  int tot = 0, pass = 0, fail = 0, skip = 0, pend = 0;
  for (int ci = 0; ci < NCOMPS; ci++)
    for (int k = 0; k < comps[ci].ncases; k++) {
      tot++;
      switch (comps[ci].cases[k].status) {
      case ST_PASS:
        pass++;
        break;
      case ST_FAIL:
        fail++;
        break;
      case ST_SKIP:
        skip++;
        break;
      default:
        pend++;
        break;
      }
    }

  printf(" %svtest%s %s— Vayu test orchestrator%s", CBOLD, CRESET, CDIM,
         CRESET);
  printf("   %s%d comps  %d cases   %s✔%d %s✘%d %s○%d %s·%d%s\n", CDIM, NCOMPS,
         tot, CGREEN, pass, CRED, fail, CYEL, skip, CDIM, pend, CRESET);
  rule();

  for (int i = 0; i < nrows; i++) {
    int ci = rows[i].c, k = rows[i].cidx;
    int is_sel = interactive && (i == sel);
    const char *cursor = is_sel ? "›" : " ";
    if (k == -1) {
      int p, r;
      status_t cs = comp_status(&comps[ci], &p, &r);
      const char *arrow = comps[ci].expanded ? "▾" : "▸";
      char kindbuf[20];
      snprintf(kindbuf, sizeof kindbuf, "[%s]", comps[ci].kind);
      printf("%s%s %s %s%-9s%s %s%-8s%s  %2d/%-2d  %s%s%s", is_sel ? CBOLD : "",
             cursor, arrow, is_sel ? CCYAN : "", comps[ci].name, CRESET, CDIM,
             kindbuf, CRESET, p, comps[ci].ncases, glyph_color(cs),
             status_label(cs), CRESET);
      if (comps[ci].note[0])
        printf("   %s%s%s", CYEL, comps[ci].note, CRESET);
      printf("\n");
    } else {
      tcase_t *tc = &comps[ci].cases[k];
      char tbuf[16];
      if (tc->status == ST_PASS || tc->status == ST_FAIL)
        snprintf(tbuf, sizeof tbuf, "%.2fms", (double)tc->time_ms);
      else if (tc->status == ST_SKIP)
        snprintf(tbuf, sizeof tbuf, "skip");
      else
        snprintf(tbuf, sizeof tbuf, "—");
      printf("%s     %s %s%s%s %-44s %s%9s%s\n", is_sel ? CBOLD : "", cursor,
             glyph_color(tc->status), glyph(tc->status), CRESET, tc->name, CDIM,
             tbuf, CRESET);
    }
  }

  rule();
  if (interactive && sel >= 0 && sel < nrows) {
    int ci = rows[sel].c, k = rows[sel].cidx;
    if (k == -1) {
      int p, r;
      status_t cs = comp_status(&comps[ci], &p, &r);
      printf(" %sdetail%s  %s%s%s %s%s%s — %d/%d ran, %d passed  %s%s%s\n",
             CDIM, CRESET, CCYAN, comps[ci].name, CRESET, CDIM, comps[ci].kind,
             CRESET, r, comps[ci].ncases, p, glyph_color(cs), status_label(cs),
             CRESET);
      printf("   %srun%s ctest --test-dir %s%s%s\n", CDIM, CRESET,
             comps[ci].test_dir, comps[ci].filter ? " -R " : "",
             comps[ci].filter ? comps[ci].filter : "");
    } else {
      tcase_t *tc = &comps[ci].cases[k];
      printf(" %sdetail%s  %s::%s   %s%s%s  %s%.2f ms%s\n", CDIM, CRESET,
             comps[ci].name, tc->name, glyph_color(tc->status),
             status_label(tc->status), CRESET, CDIM, (double)tc->time_ms,
             CRESET);
      if (tc->status == ST_FAIL && tc->detail) {
        int shown = 0;
        char *copy = strdup(tc->detail), *save = NULL;
        for (char *l = strtok_r(copy, "\n", &save); l && shown < 6;
             l = strtok_r(NULL, "\n", &save), shown++)
          printf("   %s│%s %s\n", CRED, CRESET, l);
        free(copy);
      }
    }
  }
  rule();
  if (interactive)
    printf(" %s↑/↓%s move  %sspace%s expand  %sr%s run  %sa%s run-all  %sf%s "
           "only-failed%s  %sq%s quit\n",
           CBOLD, CRESET, CBOLD, CRESET, CBOLD, CRESET, CBOLD, CRESET, CBOLD,
           CRESET, filter_failed ? " [on]" : "", CBOLD, CRESET);
}

/* ============================================================== run actions */
/* Batch (--run): build each component (streamed to stdout), then run it. */
static int run_all_batch(void) {
  int failed = 0;
  for (int i = 0; i < NCOMPS; i++) {
    if (comps[i].ncases == 0)
      continue;
    printf("==> building %s\n", comps[i].name);
    fflush(stdout);
    build_suite(&comps[i], NULL, 0);
    for (int k = 0; k < comps[i].ncases; k++)
      comps[i].cases[k].status = ST_RUNNING;
    printf("==> running %s\n", comps[i].name);
    fflush(stdout);
    failed += run_suite(&comps[i], NULL, 0);
  }
  return failed;
}

static void discover_all(void) {
  for (int i = 0; i < NCOMPS; i++)
    discover_comp(&comps[i]);
}

/* ============================================================ full-screen TUI */
#define KEY_RESIZE (-2)
static int read_key(void) {
  unsigned char c;
  ssize_t r = read(STDIN_FILENO, &c, 1);
  if (r != 1)
    return (r < 0 && errno == EINTR) ? KEY_RESIZE : 'q';
  if (c == '\x1b') {
    unsigned char seq[2];
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';
    if (seq[0] == '[') {
      if (seq[1] == 'A')
        return 'k'; /* up */
      if (seq[1] == 'B')
        return 'j'; /* down */
    }
    return '\x1b';
  }
  return c;
}

/* place cursor at (row,1) and clear that line */
static void at_clear(sb_t *s, int row) { sb_putf(s, "\x1b[%d;1H\x1b[K", row); }
static void emit_rule(sb_t *s, int row) {
  at_clear(s, row);
  sb_putf(s, "%s", CDIM);
  for (int i = 0; i < g_cols; i++)
    sb_putf(s, "─");
  sb_putf(s, "%s", CRESET);
}

/* Render one tree row (component header or case) at the given screen row. */
static void emit_tree_row(sb_t *s, int screen, const row_t *r, int is_sel) {
  at_clear(s, screen);
  const char *cur = is_sel ? "›" : " ";
  if (r->cidx == -1) {
    comp_t *c = &comps[r->c];
    int p, ran;
    status_t cs = comp_status(c, &p, &ran);
    sb_putf(s, "%s%s %s %s%-9s%s %s[%s]%s  %2d/%-2d  %s%s%s",
            is_sel ? CBOLD : "", cur, c->expanded ? "▾" : "▸",
            is_sel ? CCYAN : "", c->name, CRESET, CDIM, c->kind, CRESET, p,
            c->ncases, glyph_color(cs), status_label(cs), CRESET);
    if (c->note[0])
      sb_putf(s, "   %s%s%s", CYEL, c->note, CRESET);
  } else {
    tcase_t *tc = &comps[r->c].cases[r->cidx];
    char tb[16];
    if (tc->status == ST_PASS || tc->status == ST_FAIL)
      snprintf(tb, sizeof tb, "%.2fms", (double)tc->time_ms);
    else if (tc->status == ST_SKIP)
      snprintf(tb, sizeof tb, "skip");
    else
      snprintf(tb, sizeof tb, "—");
    sb_putf(s, "%s     %s %s%s%s %-44s %s%9s%s", is_sel ? CBOLD : "", cur,
            glyph_color(tc->status), glyph(tc->status), CRESET, tc->name, CDIM,
            tb, CRESET);
  }
}

/* a dim rule with a left-aligned label, e.g. "── log ───────────". */
static void emit_labeled_rule(sb_t *s, int row, const char *label) {
  at_clear(s, row);
  sb_putf(s, "%s── %s ", CDIM, label);
  int used = 4 + (int)strlen(label);
  for (int i = used; i < g_cols; i++)
    sb_putf(s, "─");
  sb_putf(s, "%s", CRESET);
}

/* Pane heights: detail and log shrink first on short terminals so the list
 * keeps at least one row and the footer stays pinned to g_rows. */
static void layout(int *list_h, int *detail_n, int *log_n) {
  int dn = 3, ln = 6;
  int lh = g_rows - 2 /*header*/ - 2 /*footer*/ - (1 + dn) - (1 + ln);
  while (lh < 3 && ln > 1) {
    ln--;
    lh++;
  }
  while (lh < 3 && dn > 1) {
    dn--;
    lh++;
  }
  if (lh < 1)
    lh = 1;
  *list_h = lh;
  *detail_n = dn;
  *log_n = ln;
}
static int list_height(void) {
  int lh, dn, ln;
  layout(&lh, &dn, &ln);
  return lh;
}

/* Build the whole screen into `s`, positioned absolutely (no scrolling). Reads
 * g_filter / g_status / the log ring; footer pinned to g_rows. */
static void draw_frame(sb_t *s, const row_t *rows, int nrows, int sel,
                       int top) {
  int list_h, detail_n, log_n;
  layout(&list_h, &detail_n, &log_n);

  int tot = 0, pass = 0, fail = 0, skip = 0, pend = 0;
  for (int ci = 0; ci < NCOMPS; ci++)
    for (int k = 0; k < comps[ci].ncases; k++) {
      tot++;
      switch (comps[ci].cases[k].status) {
      case ST_PASS:
        pass++;
        break;
      case ST_FAIL:
        fail++;
        break;
      case ST_SKIP:
        skip++;
        break;
      default:
        pend++;
        break;
      }
    }

  /* header */
  at_clear(s, 1);
  sb_putf(s,
          " %svtest%s %s— Vayu test orchestrator%s   %s%d comps %d cases  "
          "%s✔%d %s✘%d %s○%d %s·%d%s",
          CBOLD, CRESET, CDIM, CRESET, CDIM, NCOMPS, tot, CGREEN, pass, CRED,
          fail, CYEL, skip, CDIM, pend, CRESET);
  emit_rule(s, 2);

  /* list viewport (rows 3 .. 2+list_h) with scroll markers */
  for (int i = 0; i < list_h; i++) {
    int screen = 3 + i, idx = top + i;
    if (idx >= nrows) {
      at_clear(s, screen);
      continue;
    }
    emit_tree_row(s, screen, &rows[idx], idx == sel);
    if (i == 0 && top > 0)
      sb_putf(s, "\x1b[%d;%dH%s▲%s", screen, g_cols, CDIM, CRESET);
    if (i == list_h - 1 && top + list_h < nrows)
      sb_putf(s, "\x1b[%d;%dH%s▼%s", screen, g_cols, CDIM, CRESET);
  }

  /* detail pane: labeled rule + detail_n content lines */
  int dr = 3 + list_h;
  emit_labeled_rule(s, dr, "detail");
  char dl[6][512];
  for (int i = 0; i < detail_n; i++)
    dl[i][0] = 0;
  if (sel >= 0 && sel < nrows) {
    int ci = rows[sel].c, k = rows[sel].cidx;
    if (k == -1) {
      comp_t *c = &comps[ci];
      int p, ran;
      status_t cs = comp_status(c, &p, &ran);
      snprintf(dl[0], sizeof dl[0],
               " %s%s%s %s%s%s — %d/%d ran, %d passed  "
               "%s%s%s",
               CCYAN, c->name, CRESET, CDIM, c->kind, CRESET, ran, c->ncases, p,
               glyph_color(cs), status_label(cs), CRESET);
      if (detail_n > 1) {
        if (c->adapter == AD_PYTEST)
          snprintf(dl[1], sizeof dl[1], " %srun%s pytest %s/%s", CDIM, CRESET,
                   c->test_dir, c->filter);
        else
          snprintf(dl[1], sizeof dl[1], " %srun%s ctest --test-dir %s%s%s",
                   CDIM, CRESET, c->test_dir, c->filter ? " -R " : "",
                   c->filter ? c->filter : "");
      }
    } else {
      tcase_t *tc = &comps[ci].cases[k];
      snprintf(dl[0], sizeof dl[0], " %s::%s   %s%s%s  %s%.2f ms%s",
               comps[ci].name, tc->name, glyph_color(tc->status),
               status_label(tc->status), CRESET, CDIM, (double)tc->time_ms,
               CRESET);
      if (tc->status == ST_FAIL && tc->detail) {
        int li = 1;
        char *copy = strdup(tc->detail), *save = NULL;
        for (char *l = strtok_r(copy, "\n", &save); l && li < detail_n;
             l = strtok_r(NULL, "\n", &save), li++)
          snprintf(dl[li], sizeof dl[li], " %s│%s %s", CRED, CRESET, l);
        free(copy);
      }
    }
  }
  for (int i = 0; i < detail_n; i++) {
    at_clear(s, dr + 1 + i);
    sb_putf(s, "%s", dl[i]);
  }

  /* log pane: labeled rule + last log_n lines of build/run output */
  int lr = dr + 1 + detail_n;
  emit_labeled_rule(s, lr, "log");
  for (int i = 0; i < log_n; i++) {
    at_clear(s, lr + 1 + i);
    const char *l = log_line(g_log_n - log_n + i);
    if (l)
      sb_putf(s, " %s", l);
  }

  /* footer (pinned to the bottom two lines) */
  emit_rule(s, g_rows - 1);
  at_clear(s, g_rows);
  sb_putf(s,
          " %s↑/↓%s move  %sspace%s expand  %sr%s run  %sa%s run-all  %sf%s "
          "only-failed%s  %sq%s quit",
          CBOLD, CRESET, CBOLD, CRESET, CBOLD, CRESET, CBOLD, CRESET, CBOLD,
          CRESET, g_filter ? " [on]" : "", CBOLD, CRESET);
  if (g_status)
    sb_putf(s, "   %s%s%s", CYEL, g_status, CRESET);
  /* park the cursor out of the way (bottom-right) */
  sb_putf(s, "\x1b[%d;%dH", g_rows, g_cols);
}

/* Rebuild rows from g_filter, clamp selection/scroll, draw the whole frame. */
static void repaint(void) {
  row_t rows[256];
  int n = build_rows(rows, 256, g_filter);
  if (n == 0 && g_filter) {
    g_filter = 0;
    n = build_rows(rows, 256, 0);
  }
  if (g_sel >= n)
    g_sel = n - 1;
  if (g_sel < 0)
    g_sel = 0;
  int lh = list_height();
  if (g_sel < g_top)
    g_top = g_sel;
  if (g_sel >= g_top + lh)
    g_top = g_sel - lh + 1;
  if (g_top > n - lh)
    g_top = n - lh;
  if (g_top < 0)
    g_top = 0;
  g_frame.len = 0;
  draw_frame(&g_frame, rows, n, g_sel, g_top);
  ssize_t w = write(STDOUT_FILENO, g_frame.buf, g_frame.len);
  (void)w;
}

/* Spawn `cmd` via /bin/sh in its own process group, stream its output line by
 * line to `on_line`, and (when interactive) let the user abort with 'q' — which
 * kills the whole child group. Returns the child exit code, or -2 if aborted.
 * This is what keeps the UI responsive during a slow suite: results/log update
 * per line instead of the screen freezing until the whole run finishes. */
static int stream_exec(const char *cmd, int interactive,
                       void (*on_line)(void *, const char *), void *ctx) {
  int pfd[2];
  if (pipe(pfd) != 0)
    return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }
  if (pid == 0) {
    setpgid(0, 0); /* own group so we can kill the whole pipeline */
    dup2(pfd[1], STDOUT_FILENO);
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[0]);
    close(pfd[1]);
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }
  close(pfd[1]);

  char rbuf[4096], line[8192];
  size_t ll = 0;
  int aborted = 0;
  for (;;) {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(pfd[0], &rs);
    int maxfd = pfd[0];
    if (interactive) {
      FD_SET(STDIN_FILENO, &rs);
      if (STDIN_FILENO > maxfd)
        maxfd = STDIN_FILENO;
    }
    int sr = select(maxfd + 1, &rs, NULL, NULL, NULL);
    if (sr < 0) {
      if (errno == EINTR) { /* SIGWINCH */
        if (g_resized) {
          query_winsize();
          g_resized = 0;
          repaint();
        }
        continue;
      }
      break;
    }
    if (interactive && FD_ISSET(STDIN_FILENO, &rs)) {
      unsigned char ch;
      if (read(STDIN_FILENO, &ch, 1) == 1 && (ch == 'q' || ch == 3)) {
        aborted = 1;
        kill(-pid, SIGTERM);
        log_add("  ⨯ aborted");
      }
    }
    if (FD_ISSET(pfd[0], &rs)) {
      ssize_t r = read(pfd[0], rbuf, sizeof rbuf);
      if (r <= 0)
        break; /* EOF */
      for (ssize_t i = 0; i < r; i++) {
        char ch = rbuf[i];
        if (ch == '\n' || ll >= sizeof line - 1) {
          line[ll] = 0;
          on_line(ctx, line);
          ll = 0;
        } else if (ch != '\r') {
          line[ll++] = ch;
        }
      }
    }
  }
  if (ll) {
    line[ll] = 0;
    on_line(ctx, line);
  }
  close(pfd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (aborted)
    return -2;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* a streamed line during a build: log + repaint (interactive) or echo (batch). */
static void on_build_line(void *v, const char *line) {
  int interactive = *(int *)v;
  if (interactive) {
    log_add(line);
    repaint();
  } else {
    printf("%s\n", line);
  }
}

typedef struct {
  comp_t *c;
  int interactive;
  int failed;
  tcase_t *cur;
} run_ctx_t;
/* a streamed line during a run: parse into the model, then log/repaint or echo. */
static void on_run_line(void *v, const char *line) {
  run_ctx_t *x = v;
  if (x->c->adapter == AD_PYTEST)
    pytest_parse_line(x->c, line, &x->failed);
  else
    ctest_parse_line(x->c, line, &x->failed, &x->cur);
  if (x->interactive) {
    log_add(line);
    repaint();
  } else {
    printf("%s\n", line);
  }
}

/* Build the lazily-needed binaries (single ctest target / whole comp / pytest's
 * SITL+vsim deps), streamed. Returns the child exit code (or -2 if aborted). */
static int build_suite(comp_t *c, const char *only, int interactive) {
  char cmd[1024];
  build_cmd(cmd, sizeof cmd, c, only);
  return stream_exec(cmd, interactive, on_build_line, &interactive);
}

/* Run a component (only==NULL) or a single case, streamed; returns #failed
 * (or -2 if aborted). */
static int run_suite(comp_t *c, const char *only, int interactive) {
  char cmd[16384];
  if (c->adapter == AD_PYTEST)
    pytest_run_cmd(cmd, sizeof cmd, c, only);
  else
    ctest_run_cmd(cmd, sizeof cmd, c, only);
  run_ctx_t x = {c, interactive, 0, NULL};
  int rc = stream_exec(cmd, interactive, on_run_line, &x);
  return rc == -2 ? -2 : x.failed;
}

/* Build (lazily) then run, streaming both into the log pane. Selecting a test
 * builds just that target; a header / run-all builds the whole component. */
static void do_run_interactive(comp_t *c, const char *only) {
  if (c->ncases == 0)
    return;
  if (only) {
    tcase_t *tc = find_case(c, only);
    if (tc)
      tc->status = ST_RUNNING;
  } else {
    for (int i = 0; i < c->ncases; i++)
      c->cases[i].status = ST_RUNNING;
  }
  char bc[1024];
  build_cmd(bc, sizeof bc, c, only);
  g_status = "building… (q aborts)";
  log_addf("$ %s", bc);
  repaint();
  int brc = build_suite(c, only, 1);
  if (brc == -2) {
    g_status = NULL;
    goto reset_pending;
  }
  if (brc != 0)
    log_add("  (build exited non-zero — running anyway)");

  g_status = "running… (q aborts)";
  log_addf("$ run %s%s%s [%s]", c->name, only ? "::" : "", only ? only : "",
           c->kind);
  repaint();
  int failed = run_suite(c, only, 1);
  if (failed == -2)
    goto reset_pending;

  int p, ran;
  status_t cs = comp_status(c, &p, &ran);
  log_addf("→ %s: %s (%d/%d passed)", only ? only : c->name, status_label(cs),
           p, ran);
  g_status = NULL;
  return;

reset_pending: /* on abort, un-stick anything left mid-run */
  for (int i = 0; i < c->ncases; i++)
    if (c->cases[i].status == ST_RUNNING)
      c->cases[i].status = ST_PENDING;
  g_status = NULL;
}

static int run_interactive(void) {
  raw_enter();
  sb_init(&g_frame);
  g_sel = 0;
  g_top = 0;
  g_filter = 0;
  log_add("ready — ↑/↓ to select, r to build + run (binaries build on demand)");
  for (;;) {
    if (g_resized) {
      query_winsize();
      g_resized = 0;
    }
    repaint();

    /* rows for key handling (same view repaint just drew) */
    row_t rows[256];
    int n = build_rows(rows, 256, g_filter);

    int key = read_key();
    if (key == KEY_RESIZE)
      continue;
    if (key == 'q')
      break;
    else if (key == 'j') {
      if (g_sel + 1 < n)
        g_sel++;
    } else if (key == 'k') {
      if (g_sel > 0)
        g_sel--;
    } else if (key == ' ' && n)
      comps[rows[g_sel].c].expanded = !comps[rows[g_sel].c].expanded;
    else if (key == 'r' && n) {
      int ci = rows[g_sel].c, k = rows[g_sel].cidx;
      if (k == -1)
        do_run_interactive(&comps[ci], NULL);
      else
        do_run_interactive(&comps[ci], comps[ci].cases[k].name);
    } else if (key == 'a') {
      for (int i = 0; i < NCOMPS; i++)
        do_run_interactive(&comps[i], NULL);
    } else if (key == 'f') {
      g_filter = !g_filter;
      g_sel = 0;
      g_top = 0;
    }
  }
  sb_free(&g_frame);
  raw_restore();
  return 0;
}

static int run_report(int do_run) {
  for (int i = 0; i < NCOMPS; i++)
    comps[i].expanded = 1;
  int failed = 0;
  if (do_run)
    failed = run_all_batch();
  row_t rows[256];
  int n = build_rows(rows, 256, 0);
  printf("\n");
  render(rows, n, -1, 0, 0);
  if (do_run) {
    printf("\n %s%s%s\n", failed ? CRED : CGREEN,
           failed ? "FAILED" : "ALL PASSED", CRESET);
    return failed ? 1 : 0;
  }
  return 0;
}

int main(int argc, char **argv) {
  int do_run = 0, list_only = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--run") == 0)
      do_run = 1;
    else if (strcmp(argv[i], "--list") == 0)
      list_only = 1;
  }

  /* repo root (for absolute SITL/vsim binary paths) + pytest interpreter. */
  if (!getcwd(g_cwd, sizeof g_cwd))
    snprintf(g_cwd, sizeof g_cwd, ".");
  if (access(".venv/bin/python", X_OK) == 0)
    snprintf(g_python, sizeof g_python, "%s/.venv/bin/python", g_cwd);

  discover_all();

  if (list_only) {
    use_color = isatty(STDOUT_FILENO);
    return run_report(0);
  }
  if (do_run) {
    use_color = isatty(STDOUT_FILENO);
    return run_report(1);
  }
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    fprintf(stderr, "vtest: not a TTY — use --run (run all) or --list.\n");
    return run_report(1);
  }
  use_color = 1;
  return run_interactive();
}
