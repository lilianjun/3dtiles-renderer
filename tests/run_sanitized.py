#!/usr/bin/env python3
"""P6: run a command under ASan+LSan+UBSan and fail on any sanitizer report.

The target binary must be built with -DTILES_SANITIZE=ON (linux-asan
preset); the runtimes are enabled via environment (see sanitizer_common.py).
Fails when:
  - the exit code differs from --expect-exit (default 0), or
  - any sanitizer marker appears in stdout/stderr.

Usage:
  run_sanitized.py --suppressions <lsan.supp> [--expect-exit N] -- <cmd...>
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sanitizer_common import sanitizer_env, find_reports, maybe_wrap_xvfb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--suppressions", required=True,
                    help="LSan suppressions file (tests/lsan.supp)")
    ap.add_argument("--expect-exit", type=int, default=0)
    ap.add_argument("cmd", nargs=argparse.REMAINDER,
                    help="command to run, after --")
    args = ap.parse_args()
    if not args.cmd or args.cmd[0] != "--":
        print("FAIL: command must follow a literal --", file=sys.stderr)
        return 2
    cmd = maybe_wrap_xvfb(args.cmd[1:])
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900,
                          env=sanitizer_env(args.suppressions))
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    reports = find_reports(proc.stdout + proc.stderr)
    if reports:
        print("FAIL: sanitizer reports: %s" % ", ".join(reports))
        return 1
    if proc.returncode != args.expect_exit:
        print("FAIL: exit code %d, expected %d"
              % (proc.returncode, args.expect_exit))
        return 1
    print("PASS: sanitized run clean (exit %d)" % proc.returncode)
    return 0


if __name__ == "__main__":
    sys.exit(main())
