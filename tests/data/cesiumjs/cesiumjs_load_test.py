#!/usr/bin/env python3
"""P37-B: Bulk loading conformance test for cesium.js test data.

For each loadable tileset in INVENTORY.csv:
  - load via tiles_demo --tileset <path> --frames N
  - parse TileStats from stderr/stdout
  - record visited/loaded/failed

Usage:
  ./cesiumjs_load_test.py --demo <path/to/tiles_demo> [--limit N] [--frames N]
"""
import argparse, csv, json, os, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).parent
INVENTORY = ROOT / "INVENTORY.csv"

# Formats we support loading (from INVENTORY.md)
LOADABLE_FORMATS = {"b3dm", "pnts", "i3dm", "gltf", "glb", "cmpt", "json"}
# Features that make a tileset unloadable for us
SKIP_FEATURES = {"ext-3DTILES_content_voxels", "bv-cylinder"}

def load_inventory():
    rows = []
    with open(INVENTORY) as f:
        for r in csv.DictReader(f):
            fmts = set(r["formats"].split(";")) if r["formats"] else set()
            feats = set(r["features"].split(";")) if r["features"] else set()
            # loadable if it has at least one loadable format and no skip features
            loadable = bool(fmts & LOADABLE_FORMATS) and not (feats & SKIP_FEATURES)
            # skip pure-metadata tilesets (no renderable content)
            rows.append((r["source"], r["path"], fmts, feats, loadable))
    return rows

def run_one(demo, tileset_path, frames):
    """Run demo on a tileset, return (returncode, stats_dict, error)."""
    cmd = [demo, "--tileset", str(tileset_path), "--frames", str(frames), "--stats"]
    # headless
    if not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a"] + cmd
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return -1, {}, "timeout"
    except Exception as e:
        return -1, {}, str(e)

    # Parse stats from stdout: [stats] frame=N selected=.. loading=.. loaded=.. failed=..
    # Take the LAST frame's stats (most settled).
    stats = {}
    for line in result.stdout.splitlines():
        if line.startswith("[stats]"):
            frame_stats = {}
            for part in line.split():
                if "=" in part:
                    k, v = part.split("=", 1)
                    # strip "[stats]" prefix from first key
                    k = k.strip("[]")
                    try:
                        frame_stats[k] = int(v)
                    except ValueError:
                        pass
            stats = frame_stats
    return result.returncode, stats, result.stderr[-500:] if result.returncode != 0 else ""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--limit", type=int, default=0, help="max tilesets to test (0=all)")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--output", default=str(ROOT / "load_results.json"))
    args = ap.parse_args()

    rows = load_inventory()
    loadable = [r for r in rows if r[4]]
    skipped = [r for r in rows if not r[4]]
    print(f"Total: {len(rows)}, loadable: {len(loadable)}, skipped: {len(skipped)}")
    if args.limit:
        loadable = loadable[:args.limit]
        print(f"Limited to {args.limit}")

    results = []
    for src, rel, fmts, feats, _ in loadable:
        tp = ROOT / rel
        rc, stats, err = run_one(args.demo, tp, args.frames)
        ok = rc == 0 and stats.get("failed", 0) == 0
        results.append({
            "path": rel,
            "formats": sorted(fmts),
            "returncode": rc,
            "stats": stats,
            "ok": ok,
            "error": err[:200] if not ok else "",
        })
        status = "OK " if ok else "FAIL"
        print(f"[{status}] {rel} rc={rc} stats={stats}")

    n_ok = sum(1 for r in results if r["ok"])
    print(f"\n{n_ok}/{len(results)} passed")
    with open(args.output, "w") as f:
        json.dump(results, f, indent=1)
    print(f"Wrote {args.output}")
    return 0 if n_ok == len(results) else 1

if __name__ == "__main__":
    sys.exit(main())
