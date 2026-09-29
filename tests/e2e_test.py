#!/usr/bin/env python3
# Copyright (C) 2026 NAVRobotec Pvt Ltd
# Author: Ragnar Vallhala
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""The real vtest binary, end to end, against tests/fixture.

    e2e_test.py <path to vtest>

The fixture has a suite per adapter and per way a suite breaks. It is copied
to a temp dir so the ctest suite's configure (which vtest runs itself) never
writes into the source tree. The TUI is driven through a pty, waiting on what
the screen shows before each next key -- never on a sleep.

stdlib only; the fixture's pytest suite needs pytest installed.
"""
import os
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import fcntl
import time

VTEST = os.path.abspath(sys.argv[1])
FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixture")
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
checks = 0


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        sys.exit(f"  FAIL {what}")


def run(args, cwd, env=None, stdin=subprocess.DEVNULL):
    e = dict(os.environ, **(env or {}))
    p = subprocess.run([VTEST] + args, cwd=cwd, env=e, stdin=stdin,
                       capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout, p.stderr


class Tui:
    """vtest on a pty. expect() reads until a pattern is on screen."""

    def __init__(self, cwd, rows=30, cols=110, args=()):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ,
                        struct.pack("HHHH", rows, cols, 0, 0))
            os.chdir(cwd)
            os.execv(VTEST, [VTEST, *args])
        self.buf = b""

    def expect(self, pattern, timeout=60):
        pat = re.compile(pattern.encode())
        end = time.time() + timeout
        while True:
            m = pat.search(ANSI.sub(b"", self.buf))
            if m:
                self.buf = b""  # the next expect waits for a new frame
                return m
            left = end - time.time()
            if left <= 0:
                tail = ANSI.sub(b"", self.buf)[-1500:].decode(errors="replace")
                check(False, f"screen never showed {pattern!r}; last:\n{tail}")
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                try:
                    self.buf += os.read(self.fd, 65536)
                except OSError:  # the child closed the pty
                    check(False, f"vtest exited before {pattern!r}")

    def send(self, keys):
        os.write(self.fd, keys.encode())

    def resize(self, rows, cols):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)

    def wait(self):
        """Drain the pty until the child exits; return its exit status."""
        while True:
            try:
                if not os.read(self.fd, 65536):
                    break
            except OSError:
                break
        _, st = os.waitpid(self.pid, 0)
        os.close(self.fd)
        return os.waitstatus_to_exitcode(st)


def cli(repo):
    rc, out, _ = run(["--version"], repo)
    check(rc == 0 and out.startswith("vtest "), "--version")
    for flag in ("--help", "-h"):
        rc, out, _ = run([flag], repo)
        check(rc == 0 and "usage: vtest" in out, flag)
    rc, _, err = run(["--bogus"], repo)
    check(rc == 2 and "unknown argument '--bogus'" in err, "unknown argument")
    rc, _, err = run(["--conf"], repo)
    check(rc == 2 and "unknown argument '--conf'" in err, "--conf needs a path")

    # the title comes from the cwd; a cwd that is / or is gone still works
    rc, _, err = run(["--conf", "/nonexistent.conf"], "/")
    check(rc == 2 and "no /nonexistent.conf here" in err, "run from /")
    p = subprocess.run(["sh", "-c", 'd=$(mktemp -d) && cd "$d" && rmdir "$d" '
                        '&& exec "$0" --list', VTEST], capture_output=True,
                       text=True)
    check(p.returncode == 2 and "no vtest.conf here" in p.stderr,
          "run from a deleted directory")

    empty = tempfile.mkdtemp()
    rc, _, err = run(["--list"], empty)
    check(rc == 2 and "no vtest.conf here" in err, "no conf in the cwd")
    with open(os.path.join(empty, "bad.conf"), "w") as f:
        f.write("[a]\nnope = 1\n")
    rc, _, err = run(["--conf", "bad.conf", "--list"], empty)
    check(rc == 2 and "unknown key 'nope'" in err, "a conf with an error")
    with open(os.path.join(empty, "none.conf"), "w") as f:
        f.write("# nothing\n")
    rc, _, err = run(["--list"], empty, env={"VTEST_CONF": "none.conf"})
    check(rc == 2 and "declares no suites" in err, "VTEST_CONF, no suites")
    shutil.rmtree(empty)


def batch(repo):
    rc, out, _ = run(["--list"], repo)
    check(rc == 0, "--list succeeds whatever the suites hold")
    check("[ctest]" in out and "[pytest]" in out and "[check]" in out,
          "--list shows every adapter")
    check("not built — build the unbuilt target" in out,
          "an unconfigured dir without configure = says so")
    check("no cmd = in vtest.conf" in out, "a check without a cmd says so")
    check(os.path.isdir(os.path.join(repo, "build")),
          "discovery ran the ct suite's configure")

    rc, out, _ = run(["--run"], repo)
    check(rc == 1 and "FAILED" in out, "--run fails on the failures")
    check("gate ran" in out, "the check's output streams through")
    check(re.search(r"ct\s+\[ctest\]\s+1/3\s+FAIL", out), "ct: 1 of 3")
    check(re.search(r"py\s+\[pytest\]\s+2/5\s+FAIL", out), "py: 2 of 5")
    check(re.search(r"hil\s+\[check\]\s+0/1\s+PASS", out),
          "exit 77 is a skip, and a skip is not a failure")
    check("unbuilt: no cases discovered" in out, "no cases is a failure")

    # not a terminal and no mode: a report, not a hang
    rc, out, err = run([], repo)
    check(rc == 1 and "not a TTY" in err, "no TTY falls back to --run")
    m, sl = pty.openpty()
    rc, out, err = run([], repo, stdin=sl)
    os.close(m)
    os.close(sl)
    check(rc == 1 and "not a TTY" in err, "a TTY on stdin only is not enough")

    # a passing catalog passes; a repo .venv is the interpreter
    ok = tempfile.mkdtemp()
    with open(os.path.join(ok, "vtest.conf"), "w") as f:
        f.write("[g]\nadapter = check\ncmd = true\n")
    os.makedirs(os.path.join(ok, ".venv", "bin"))
    os.symlink(sys.executable, os.path.join(ok, ".venv", "bin", "python"))
    rc, out, _ = run(["--run"], ok)
    check(rc == 0 and "ALL PASSED" in out, "all passing is exit 0")
    shutil.rmtree(ok)


def colour(repo):
    t = Tui(repo, args=("--list",))
    t.expect(r"test orchestrator")
    check(t.wait() == 0, "--list on a terminal, in colour")
    t = Tui(repo, args=("--run",))
    t.expect(r"FAILED")
    check(t.wait() == 1, "--run on a terminal")


def tui(repo):
    t = Tui(repo)
    t.expect(r"ready — ")
    t.send("\x1b[B")          # down to ok
    t.expect(r"ct::ok")
    t.send("r")               # run one case
    t.expect(r"→ ok: PASS")
    t.send("\x1b[B")          # down to bad
    t.send("r")
    t.expect(r"→ bad: FAIL")
    t.send("\x1b[C")          # focus detail
    t.expect(r"pane \[detail\]")
    t.send("+++")
    t.expect(r"boom")
    t.send("\x1b[C")          # focus log, page it
    t.expect(r"pane \[log\]")
    t.send("\x1b[5~")
    t.expect(r"log  ▲")
    t.send("\x1b[6~")
    t.send("\x1b[C")          # back to the list
    t.expect(r"pane \[tests\]")
    t.send("f")               # only failures
    t.expect(r"failed \[on\]")
    t.send("f")
    t.resize(14, 80)          # a resize redraws at the new size
    t.expect(r"q quit")
    t.resize(30, 110)
    t.expect(r"q quit")

    t.send("a")               # run everything
    t.expect(r"→ nocmd: FAIL", timeout=120)

    t.send("q")
    check(t.wait() == 0, "q leaves the TUI with exit 0")

    # abort mid-run, and SIGTERM mid-run takes the suite down with vtest
    slow = tempfile.mkdtemp()
    with open(os.path.join(slow, "vtest.conf"), "w") as f:
        f.write("[slow]\nadapter = check\ncmd = echo started; sleep 30\n")
    t = Tui(slow)
    t.expect(r"ready — ")
    t.send("r")
    t.expect(r"started")
    t.send("q")
    t.expect(r"aborted")
    t.send("r")
    t.expect(r"started")
    os.kill(t.pid, signal.SIGTERM)
    check(t.wait() == 1, "SIGTERM exits 1")
    out = subprocess.run(["pgrep", "-f", "sleep 30"], capture_output=True)
    check(out.returncode != 0, "the suite did not outlive vtest")
    shutil.rmtree(slow)


def main():
    repo = tempfile.mkdtemp(prefix="vtest_e2e_")
    shutil.copytree(FIXTURE, repo, dirs_exist_ok=True)
    try:
        cli(repo)
        batch(repo)
        colour(repo)
        tui(repo)
    finally:
        shutil.rmtree(repo, ignore_errors=True)
    print(f"  {checks} checks, 0 failures")


if __name__ == "__main__":
    main()
