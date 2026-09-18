#!/usr/bin/env sh
# Count lines of source in this repo, by extension, newest-largest first.
#
#   loc.sh [dir ...]       default: the working directory
#
# Skips build directories, .git, and vendored trees (extern/, third_party/,
# node_modules/) -- the point is the code this repo is responsible for, not the
# code it pins. Always exits 0: this is a report, not a gate.
set -eu
[ $# -gt 0 ] || set -- .

find "$@" \
  -type d \( -name '.git' -o -name 'extern' -o -name 'third_party' \
             -o -name 'node_modules' -o -name 'build*' -o -name '.venv' \) -prune -o \
  -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \
             -o -name '*.py' -o -name '*.sh' -o -name '*.cmake' \
             -o -name 'CMakeLists.txt' \) -print \
| while IFS= read -r f; do
    printf '%s %s\n' "$(wc -l < "$f")" "${f##*.}"
  done \
| awk '
    { lines[$2] += $1; files[$2]++; total += $1; nfiles++ }
    END {
      for (e in lines)
        printf "  %-6s %7d lines  %4d files\n", e, lines[e], files[e]
      printf "  %-6s %7d lines  %4d files\n", "TOTAL", total, nfiles
    }' \
| sort -k2 -rn
