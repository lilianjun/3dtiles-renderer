#!/usr/bin/env python3
"""P6 fault-injection test: bad inputs must fail gracefully, never crash.

Driven through tiles_demo; the exit-code contract is:
  A. nonexistent tileset path -> exit 1 (clean error message, no crash)
  B. corrupt tileset.json      -> exit 1 (bounded load wait, no crash)
  C. corrupt .glb tile content -> exit 0 (bad tile skipped, rest renders)

A crash (segfault/abort, e.g. exit -11/-6) fails the test. When the demo
binary was built with -DTILES_SANITIZE=ON, wrap this script in
run_sanitized.py to also gate on ASan/LSan/UBSan reports.

Usage:
  fault_test.py --demo <tiles_demo> [--workdir <tmpdir>]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sanitizer_common import find_reports, maybe_wrap_xvfb

HERE = os.path.dirname(os.path.abspath(__file__))


def run_demo(demo, args, timeout=600):
    cmd = maybe_wrap_xvfb(
        [demo, "--width", "800", "--height", "600"] + args)
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          timeout=timeout)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    reports = find_reports(proc.stdout + proc.stderr)
    if reports:
        print("FAIL: sanitizer reports: %s" % ", ".join(reports))
        return None
    return proc.returncode


def crashed(returncode):
    # Unix: negative = killed by signal (e.g. -11 SIGSEGV, -6 SIGABRT).
    # Windows: 0xC0000005 etc. Either way, anything outside {0, 1} here is
    # a crash, not a graceful failure.
    return returncode not in (0, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--workdir", default=None)
    args = ap.parse_args()

    workdir = args.workdir or tempfile.mkdtemp(prefix="fault_test_")
    os.makedirs(workdir, exist_ok=True)
    failures = 0

    # Case A: nonexistent tileset path.
    print("=== case A: nonexistent tileset path ===")
    rc = run_demo(args.demo, ["--frames", "5",
                              "--tileset", "/nonexistent/tileset.json"])
    if rc is None:
        failures += 1
    elif crashed(rc):
        print("FAIL: case A crashed (exit %d)" % rc)
        failures += 1
    elif rc != 1:
        print("FAIL: case A: expected graceful exit 1, got %d" % rc)
        failures += 1
    else:
        print("PASS: case A (graceful exit 1)")

    # Case B: corrupt tileset.json (invalid JSON). The SDK's load() waits a
    # bounded 30 s for the root tile, then fails gracefully.
    print("=== case B: corrupt tileset.json ===")
    bad_dir = os.path.join(workdir, "bad_json")
    os.makedirs(bad_dir, exist_ok=True)
    with open(os.path.join(bad_dir, "tileset.json"), "w") as f:
        f.write("{ this is not valid json !!!")
    rc = run_demo(args.demo, ["--frames", "5",
                              "--tileset",
                              os.path.join(bad_dir, "tileset.json")],
                  timeout=300)
    if rc is None:
        failures += 1
    elif crashed(rc):
        print("FAIL: case B crashed (exit %d)" % rc)
        failures += 1
    elif rc != 1:
        print("FAIL: case B: expected graceful exit 1, got %d" % rc)
        failures += 1
    else:
        print("PASS: case B (graceful exit 1)")

    # Case C: corrupt .glb tile content. Copy the vendored tileset, smash
    # one child glb with garbage bytes; the bad tile must be skipped while
    # the rest still renders (exit 0).
    print("=== case C: corrupt .glb tile content ===")
    corrupt_dir = os.path.join(workdir, "corrupt_glb")
    shutil.rmtree(corrupt_dir, ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "data", "p3_box_tileset"), corrupt_dir)
    with open(os.path.join(corrupt_dir, "child_a.glb"), "wb") as f:
        f.write(b"\x00\xff not a glb at all, just garbage bytes \xde\xad" * 64)
    shot = os.path.join(workdir, "corrupt_glb.png")
    rc = run_demo(args.demo, ["--frames", "30",
                              "--tileset",
                              os.path.join(corrupt_dir, "tileset.json"),
                              "--screenshot", shot])
    if rc is None:
        failures += 1
    elif crashed(rc):
        print("FAIL: case C crashed (exit %d)" % rc)
        failures += 1
    elif rc != 0:
        print("FAIL: case C: expected exit 0 (bad tile skipped), got %d" % rc)
        failures += 1
    elif not os.path.exists(shot):
        print("FAIL: case C: screenshot not written")
        failures += 1
    else:
        print("PASS: case C (corrupt tile skipped, rest rendered)")

    if failures:
        print("FAIL: fault_test (%d case(s) failed)" % failures)
        return 1
    print("PASS: fault_test (all cases graceful)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
