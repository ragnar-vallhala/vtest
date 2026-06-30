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
#define _POSIX_C_SOURCE 200809L /* popen/pclose, strtok_r, strdup under -std=c11 */
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define VT_W 78
#define VT_NAME 128

typedef enum { ST_PENDING, ST_RUNNING, ST_PASS, ST_FAIL, ST_SKIP } status_t;

typedef struct {
  char name[VT_NAME];
  status_t status;
  float time_ms;
  char *detail; /* captured failure output (heap), or NULL */
} tcase_t;

typedef struct {
  const char *name;     /* component label */
  const char *kind;     /* "ctest" */
  const char *test_dir; /* ctest --test-dir <dir> */
  const char *filter;   /* -R <regex>, or NULL for all */
  tcase_t *cases;
  int ncases;
  int expanded;
  char note[160]; /* e.g. "not built — run: vayu.sh build sitl" */
} comp_t;

/* The catalog: exactly the ctest suites vayu.sh drives. */
static comp_t comps[] = {
    {"sitl", "ctest", "build_sitl", NULL, NULL, 0, 1, ""},
    {"gcs", "ctest", "navigator/build", "^tst_", NULL, 0, 0, ""},
};
static const int NCOMPS = (int)(sizeof comps / sizeof comps[0]);

/* ============================================================ ctest adapter */

/* Run `cmd` (with 2>&1) and return its full stdout as a heap string (caller
 * frees), or NULL on spawn failure. */
static char *run_capture(const char *cmd) {
  FILE *fp = popen(cmd, "r");
  if (!fp) return NULL;
  size_t cap = 8192, len = 0;
  char *buf = malloc(cap);
  if (!buf) { pclose(fp); return NULL; }
  size_t n;
  char tmp[2048];
  while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) {
    if (len + n + 1 > cap) {
      while (len + n + 1 > cap) cap *= 2;
      char *nb = realloc(buf, cap);
      if (!nb) break;
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
  if (!p) return 0;
  const char *hash = strchr(p, '#');
  const char *colon = strchr(p, ':');
  if (!hash || !colon || hash > colon) return 0;
  const char *q = colon + 1;
  while (*q == ' ') q++;
  size_t len = strlen(q);
  while (len && (q[len - 1] == '\n' || q[len - 1] == '\r' || q[len - 1] == ' '))
    len--;
  if (!len) return 0;
  if (len >= namesz) len = namesz - 1;
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
  if (!p) return 0;
  const char *hash = strchr(p, '#');
  const char *colon = hash ? strchr(hash, ':') : NULL;
  if (!hash || !colon) return 0;
  p = colon + 1;
  while (*p == ' ') p++;
  /* name runs up to " ." (space then the leader dots) */
  const char *e = p;
  while (*e && !(e[0] == ' ' && e[1] == '.')) e++;
  if (!*e) return 0; /* no dots -> not a result line */
  size_t len = (size_t)(e - p);
  while (len && p[len - 1] == ' ') len--;
  if (len >= namesz) len = namesz - 1;
  memcpy(name, p, len);
  name[len] = 0;

  if (strstr(line, "Passed")) *st = ST_PASS;
  else if (strstr(line, "Skipped")) *st = ST_SKIP;
  else if (strstr(line, "Failed")) *st = ST_FAIL;
  else *st = ST_FAIL;

  *ms = 0.0f;
  const char *s = strstr(line, "sec");
  if (s) {
    const char *q = s;
    while (q > line && q[-1] == ' ') q--;
    const char *numend = q;
    while (q > line && (isdigit((unsigned char)q[-1]) || q[-1] == '.')) q--;
    size_t nl = (size_t)(numend - q);
    char num[32];
    if (nl && nl < sizeof num) {
      memcpy(num, q, nl);
      num[nl] = 0;
      *ms = (float)atof(num) * 1000.0f;
    }
  }
  return 1;
}

static tcase_t *find_case(comp_t *c, const char *name) {
  for (int i = 0; i < c->ncases; i++)
    if (strcmp(c->cases[i].name, name) == 0) return &c->cases[i];
  return NULL;
}

/* Append one line (cap the buffer so a chatty failure can't run away). */
static void detail_append(char **d, const char *line) {
  size_t cur = *d ? strlen(*d) : 0;
  if (cur > 1400) return;
  size_t add = strlen(line) + 2;
  char *nb = realloc(*d, cur + add + 1);
  if (!nb) return;
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
        arr = realloc(arr, (size_t)cap * sizeof *arr);
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

/* Run a component (only==NULL) or a single case (only==name) via ctest, and
 * fold the parsed results back into the model. Returns #failed. */
static int ctest_run(comp_t *c, const char *only) {
  char cmd[640];
  char rflag[256] = "";
  if (only)
    snprintf(rflag, sizeof rflag, " -R '^%s$'", only);
  else if (c->filter)
    snprintf(rflag, sizeof rflag, " -R '%s'", c->filter);
  snprintf(cmd, sizeof cmd, "ctest --test-dir '%s'%s --output-on-failure 2>&1",
           c->test_dir, rflag);

  char *out = run_capture(cmd);
  if (!out) return 0;

  int failed = 0;
  tcase_t *cur = NULL; /* accumulate output into a failing case's detail */
  char *save = NULL;
  for (char *line = strtok_r(out, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
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
        if (st == ST_FAIL) failed++;
      }
      cur = (tc && st == ST_FAIL) ? tc : NULL;
    } else if (cur) {
      if (strstr(line, "tests passed") ||
          strstr(line, "The following tests FAILED") ||
          strstr(line, "Total Test time"))
        cur = NULL;
      else
        detail_append(&cur->detail, line);
    }
  }
  free(out);
  return failed;
}

/* ============================================================== terminal/ui */
static int use_color = 0;
static struct termios saved_tio;
static int raw_active = 0;

static const char *C(const char *code) { return use_color ? code : ""; }
#define CRESET C("\x1b[0m")
#define CBOLD C("\x1b[1m")
#define CDIM C("\x1b[2m")
#define CGREEN C("\x1b[32m")
#define CRED C("\x1b[31m")
#define CYEL C("\x1b[33m")
#define CCYAN C("\x1b[36m")

static void raw_restore(void) {
  if (raw_active) {
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
    printf("\x1b[?25h");
    fflush(stdout);
    raw_active = 0;
  }
}
static void on_signal(int sig) {
  (void)sig;
  raw_restore();
  _exit(1);
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
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  printf("\x1b[?25l");
}

static const char *glyph(status_t s) {
  switch (s) {
    case ST_PASS: return "✔";
    case ST_FAIL: return "✘";
    case ST_RUNNING: return "…";
    case ST_SKIP: return "○";
    default: return "·";
  }
}
static const char *glyph_color(status_t s) {
  switch (s) {
    case ST_PASS: return CGREEN;
    case ST_FAIL: return CRED;
    case ST_RUNNING: return CYEL;
    case ST_SKIP: return CYEL;
    default: return CDIM;
  }
}
static const char *status_label(status_t s) {
  switch (s) {
    case ST_PASS: return "PASS";
    case ST_FAIL: return "FAIL";
    case ST_SKIP: return "SKIP";
    default: return "----";
  }
}

static status_t comp_status(const comp_t *c, int *passed, int *ran) {
  int p = 0, r = 0, fail = 0;
  for (int i = 0; i < c->ncases; i++) {
    status_t st = c->cases[i].status;
    if (st == ST_PASS) { p++; r++; }
    else if (st == ST_FAIL) { fail++; r++; }
    else if (st == ST_SKIP) { r++; }
  }
  *passed = p; *ran = r;
  if (fail) return ST_FAIL;
  if (r == c->ncases && c->ncases > 0) return ST_PASS;
  return ST_PENDING;
}

typedef struct { int c, cidx; } row_t;
static int build_rows(row_t *rows, int maxrows, int filter_failed) {
  int n = 0;
  for (int ci = 0; ci < NCOMPS && n < maxrows; ci++) {
    rows[n].c = ci; rows[n].cidx = -1; n++;
    if (comps[ci].expanded)
      for (int k = 0; k < comps[ci].ncases && n < maxrows; k++) {
        if (filter_failed && comps[ci].cases[k].status != ST_FAIL) continue;
        rows[n].c = ci; rows[n].cidx = k; n++;
      }
  }
  return n;
}

static void rule(void) {
  printf(" %s", CDIM);
  for (int i = 0; i < VT_W; i++) printf("─");
  printf("%s\n", CRESET);
}

static void render(const row_t *rows, int nrows, int sel, int filter_failed,
                   int interactive) {
  int tot = 0, pass = 0, fail = 0, skip = 0, pend = 0;
  for (int ci = 0; ci < NCOMPS; ci++)
    for (int k = 0; k < comps[ci].ncases; k++) {
      tot++;
      switch (comps[ci].cases[k].status) {
        case ST_PASS: pass++; break;
        case ST_FAIL: fail++; break;
        case ST_SKIP: skip++; break;
        default: pend++; break;
      }
    }

  printf(" %svtest%s %s— Vayu test orchestrator%s", CBOLD, CRESET, CDIM, CRESET);
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
      printf(" %sdetail%s  %s%s%s %s%s%s — %d/%d ran, %d passed  %s%s%s\n", CDIM,
             CRESET, CCYAN, comps[ci].name, CRESET, CDIM, comps[ci].kind, CRESET,
             r, comps[ci].ncases, p, glyph_color(cs), status_label(cs), CRESET);
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
static int run_all_live(void) {
  int failed = 0;
  for (int i = 0; i < NCOMPS; i++) {
    for (int k = 0; k < comps[i].ncases; k++) comps[i].cases[k].status = ST_RUNNING;
    failed += ctest_run(&comps[i], NULL);
  }
  return failed;
}

static void discover_all(void) {
  for (int i = 0; i < NCOMPS; i++) ctest_discover(&comps[i]);
}

/* =================================================================== modes */
static int read_key(void) {
  unsigned char c;
  if (read(STDIN_FILENO, &c, 1) != 1) return -1;
  if (c == '\x1b') {
    unsigned char seq[2];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    if (seq[0] == '[') {
      if (seq[1] == 'A') return 'k';
      if (seq[1] == 'B') return 'j';
    }
    return '\x1b';
  }
  return c;
}

static int run_interactive(void) {
  raw_enter();
  int sel = 0, filter_failed = 0;
  for (;;) {
    row_t rows[256];
    int n = build_rows(rows, 256, filter_failed);
    if (n == 0) { filter_failed = 0; n = build_rows(rows, 256, 0); }
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;

    printf("\x1b[2J\x1b[H");
    render(rows, n, sel, filter_failed, 1);
    fflush(stdout);

    int key = read_key();
    if (key == 'q') break;
    else if (key == 'j') sel = (sel + 1 < n) ? sel + 1 : sel;
    else if (key == 'k') sel = (sel > 0) ? sel - 1 : 0;
    else if (key == ' ')
      comps[rows[sel].c].expanded = !comps[rows[sel].c].expanded;
    else if (key == 'r') {
      int ci = rows[sel].c, k = rows[sel].cidx;
      /* show a "running" frame before the (blocking) ctest call */
      if (k == -1)
        for (int i = 0; i < comps[ci].ncases; i++)
          comps[ci].cases[i].status = ST_RUNNING;
      else
        comps[ci].cases[k].status = ST_RUNNING;
      printf("\x1b[2J\x1b[H");
      render(rows, n, sel, filter_failed, 1);
      printf("\n  %srunning %s…%s\n", CYEL, comps[ci].name, CRESET);
      fflush(stdout);
      if (k == -1) ctest_run(&comps[ci], NULL);
      else ctest_run(&comps[ci], comps[ci].cases[k].name);
    } else if (key == 'a') {
      printf("\x1b[2J\x1b[H");
      printf("\n  %srunning all suites…%s\n", CYEL, CRESET);
      fflush(stdout);
      run_all_live();
    } else if (key == 'f') {
      filter_failed = !filter_failed;
      sel = 0;
    }
  }
  raw_restore();
  printf("bye.\n");
  return 0;
}

static int run_report(int do_run) {
  for (int i = 0; i < NCOMPS; i++) comps[i].expanded = 1;
  int failed = 0;
  if (do_run) failed = run_all_live();
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
    if (strcmp(argv[i], "--run") == 0) do_run = 1;
    else if (strcmp(argv[i], "--list") == 0) list_only = 1;
  }

  discover_all();

  if (list_only) { use_color = isatty(STDOUT_FILENO); return run_report(0); }
  if (do_run) { use_color = isatty(STDOUT_FILENO); return run_report(1); }
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    fprintf(stderr, "vtest: not a TTY — use --run (run all) or --list.\n");
    return run_report(1);
  }
  use_color = 1;
  return run_interactive();
}
