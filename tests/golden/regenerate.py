#!/usr/bin/env python3
"""Regenerate the frozen golden screenshots in tests/golden/.

Re-renders every MANIFEST entry from tests/golden_test.py with the fixed
parameters and overwrites the golden PNGs. Run from the repo root:

  python3 tests/golden/regenerate.py --demo build/linux/tiles_demo --confirm

WHEN IT IS OK TO REGENERATE:
  - The render pipeline changed *intentionally* (new Filament/cesium
    version, deliberate visual change, camera/framing change). The diff
    must be human-reviewed first: render, inspect every changed image
    yourself, and explain the change in the commit message (or an ADR).
  - A golden was frozen from a bad render (should never happen: goldens
    are only frozen from renders the phase's own pixel assertions
    already accepted).

WHEN IT IS NOT OK:
  - To make a failing golden_regression test pass without understanding
    the diff. That defeats the entire purpose of the gate.

The script refuses to run without --confirm, prints the md5 of every
golden before and after, and lists exactly which images changed.
"""
import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
sys.path.insert(0, TESTS)

import golden_test  # noqa: E402


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--confirm", action="store_true",
                    help="required: acknowledge you reviewed the policy above")
    args = ap.parse_args()
    if not args.confirm:
        print("Refusing: pass --confirm after reading the policy in this "
              "file's docstring.", flush=True)
        return 2

    before = {}
    for name, _, _ in golden_test.MANIFEST:
        p = os.path.join(HERE, name + ".png")
        before[name] = md5(p) if os.path.exists(p) else "<missing>"
    for name, _, _, _ in golden_test.TRAJECTORY_GOLDENS:
        p = os.path.join(HERE, name + ".png")
        before[name] = md5(p) if os.path.exists(p) else "<missing>"

    import tempfile
    with tempfile.TemporaryDirectory(prefix="golden_regen_") as work:
        for name, tileset_dir, frames in golden_test.MANIFEST:
            out = os.path.join(work, name + ".png")
            err = golden_test.render_demo(args.demo, tileset_dir, out, frames)
            if err is not None:
                print("RENDER FAILED for %s: %s" % (name, err), flush=True)
                return 1
            dst = os.path.join(HERE, name + ".png")
            with open(out, "rb") as fsrc, open(dst, "wb") as fdst:
                fdst.write(fsrc.read())
        for name, tileset_dir, traj_csv, frame_index in \
                golden_test.TRAJECTORY_GOLDENS:
            out = os.path.join(work, name + ".png")
            err = golden_test.render_trajectory(args.demo, tileset_dir,
                                                traj_csv, frame_index, out)
            if err is not None:
                print("RENDER FAILED for %s: %s" % (name, err), flush=True)
                return 1
            dst = os.path.join(HERE, name + ".png")
            with open(out, "rb") as fsrc, open(dst, "wb") as fdst:
                fdst.write(fsrc.read())

    print("%-16s %-32s %-32s %s" % ("golden", "before", "after", "changed"),
          flush=True)
    changed = 0
    for name in before:
        after = md5(os.path.join(HERE, name + ".png"))
        ch = "YES" if after != before[name] else "no"
        changed += (after != before[name])
        print("%-16s %-32s %-32s %s" % (name, before[name], after, ch),
              flush=True)
    print("%d/%d goldens changed." % (changed, len(before)), flush=True)
    print("Next: inspect every changed image, then commit with an "
          "explanation (see docstring).", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
