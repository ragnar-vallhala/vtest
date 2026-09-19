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
 * @file pane_scroll_test.c
 * @brief Windowing and scroll clamps for the log and detail panes.
 *
 * vtest is one file of statics, so the unit under test is reached by including
 * it with main() renamed away. That keeps the production file free of test
 * scaffolding and needs no header extraction for logic that is not a library.
 *
 * What is worth asserting here is the arithmetic that decides which lines the
 * log pane shows: it is off-by-one-prone, it wraps a ring buffer, and it is
 * invisible in a TUI until someone scrolls to the wrong place.
 */
#define main vtest_main
#include "../vtest.c"
#undef main

#include <assert.h>

static int checks = 0;
#define CHECK(c, what)                                                         \
  do {                                                                         \
    checks++;                                                                  \
    if (!(c)) {                                                                \
      printf("  FAIL %s\n", what);                                             \
      return 1;                                                                \
    }                                                                          \
  } while (0)

/* The pane shows `visible` lines ending `off` back from the newest. */
static const char *shown(int visible, int i) {
  return log_line(g_log_n - visible - g_log_off + i);
}

int main(void) {
  const int VIS = 6;

  for (int i = 0; i < 20; i++) {
    char b[32];
    snprintf(b, sizeof b, "line%d", i);
    log_add(b);
  }
  CHECK(g_log_n == 20, "20 lines held");
  CHECK(g_log_off == 0, "a fresh ring follows the tail");

  /* At the tail the window ends on the newest line. */
  CHECK(!strcmp(shown(VIS, VIS - 1), "line19"), "tail shows the newest line");
  CHECK(!strcmp(shown(VIS, 0), "line14"), "tail window starts VIS-1 back");

  /* Scrolling back moves the whole window, not just the end. */
  log_scroll(5, VIS);
  CHECK(g_log_off == 5, "scrolled back 5");
  CHECK(!strcmp(shown(VIS, VIS - 1), "line14"), "window end moved back 5");
  CHECK(!strcmp(shown(VIS, 0), "line9"), "window start moved back 5");

  /* New output must not drag a scrolled view along with it. */
  log_add("line20");
  CHECK(g_log_off == 6, "offset grows with new lines while scrolled");
  CHECK(!strcmp(shown(VIS, VIS - 1), "line14"), "same lines stay on screen");

  /* Back at the tail, following resumes. */
  log_scroll(-100, VIS);
  CHECK(g_log_off == 0, "scrolling past the tail clamps to 0");
  CHECK(!strcmp(shown(VIS, VIS - 1), "line20"), "tail follows new output again");
  log_add("line21");
  CHECK(g_log_off == 0, "at the tail the offset stays put");
  CHECK(!strcmp(shown(VIS, VIS - 1), "line21"), "and the newest line shows");

  /* Cannot scroll past the oldest line held. */
  log_scroll(10000, VIS);
  CHECK(g_log_off == g_log_n - VIS, "scrolling past the start clamps");
  CHECK(!strcmp(shown(VIS, 0), "line0"), "the oldest line is reachable");

  /* A ring shorter than the pane has nothing to scroll. */
  g_log_n = 3;
  g_log_off = 0;
  log_scroll(5, VIS);
  CHECK(g_log_off == 0, "no scroll when the log is shorter than the pane");

  /* --- the detail pane scrolls on the same contract ------------------------ */
  g_dl_n = 20;
  g_detail_off = 0;
  detail_scroll(4, VIS);
  CHECK(g_detail_off == 4, "detail scrolls forward");
  detail_scroll(-100, VIS);
  CHECK(g_detail_off == 0, "detail clamps at the top");
  detail_scroll(10000, VIS);
  CHECK(g_detail_off == 20 - VIS, "detail clamps at the bottom");

  /* Re-clamping after the content shrinks is what stops a stale offset from
   * showing an empty pane when the selection moves to a shorter entry. */
  g_dl_n = 2;
  detail_scroll(0, VIS);
  CHECK(g_detail_off == 0, "a shorter entry re-clamps the offset");

  g_dl_n = 0;
  g_detail_off = 0;
  detail_scroll(3, VIS);
  CHECK(g_detail_off == 0, "nothing to scroll when the pane is empty");

  printf("  %d checks, 0 failures\n", checks);
  return 0;
}
