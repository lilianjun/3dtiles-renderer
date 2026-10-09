#!/usr/bin/env python3
"""
P37 Step 3: Conformance test - render with our engine using benchmark params,
then compare with Cesium.js benchmark.

Usage:
  python3 conformance_test.py --benchmark <dir> --tileset <tileset.json>
                              --demo <tiles_demo_exe> [--out <dir>]

  benchmark dir contains: render.png (cesium.js), params.json
  Output: ours.png, compare.json, diff.png
"""
import argparse
import json
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--benchmark', required=True, help='Benchmark dir (render.png + params.json)')
    ap.add_argument('--tileset', required=True, help='Tileset JSON path')
    ap.add_argument('--demo', required=True, help='tiles_demo executable')
    ap.add_argument('--out', default='/tmp/conformance_out', help='Output dir')
    ap.add_argument('--frames', type=int, default=60, help='Frames to render')
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    # Read benchmark params.
    with open(os.path.join(args.benchmark, 'params.json')) as f:
        params = json.load(f)

    cam = params['camera']
    bg = params['backgroundColor']

    # Build demo command.
    # NOTE: The renderer does the local-origin rebase internally
    # (renderer.cpp subtracts origin from camera coords).
    # So we pass ECEF coordinates directly, NOT local.
    eye = cam['position']  # ECEF, renderer will rebase
    # Target: tileset center in ECEF. We don't know it directly, but
    # the camera direction points at it. Use eye + direction * dist.
    # For now, use the probe to get local origin, then target = origin
    # (tileset center is at local origin by P5 rebase construction).
    import re
    probe_cmd = ['xvfb-run', '-a', args.demo,
                 '--tileset', args.tileset,
                 '--width', '64', '--height', '64',
                 '--frames', '1']
    probe = subprocess.run(probe_cmd, capture_output=True, text=True, timeout=120)
    m = re.search(r'local origin: ([\d.e+-]+), ([\d.e+-]+), ([\d.e+-]+)',
                  probe.stdout + probe.stderr)
    if m:
        origin = [float(m.group(1)), float(m.group(2)), float(m.group(3))]
        print(f"Local origin: {origin}", flush=True)
    else:
        print("WARNING: could not parse local origin", flush=True)
        origin = [0, 0, 0]
    # Target in ECEF = origin (tileset center)
    # But to match Cesium.js exactly, use eye + direction * distance.
    # Distance = |eye - origin| (origin is tileset center).
    import math
    eye_ecef = cam['position']
    direction = cam['direction']
    dx = eye_ecef[0] - origin[0]
    dy = eye_ecef[1] - origin[1]
    dz = eye_ecef[2] - origin[2]
    dist = math.sqrt(dx*dx + dy*dy + dz*dz)
    print(f"Camera distance to tileset: {dist:.2f}m", flush=True)
    target = [
        eye_ecef[0] + direction[0] * dist,
        eye_ecef[1] + direction[1] * dist,
        eye_ecef[2] + direction[2] * dist,
    ]
    eye = eye_ecef  # ECEF, renderer will rebase
    up = cam['up']

    # P37: Cesium frustum.fov is HORIZONTAL FOV, Filament expects VERTICAL.
    # Convert: tan(fovx/2) = tan(fovy/2) * aspect  =>  fovy = 2*atan(tan(fovx/2)/aspect)
    import math
    fovx_deg = cam['fov']
    aspect = cam['aspectRatio']
    fovx_rad = math.radians(fovx_deg)
    fovy_rad = 2 * math.atan(math.tan(fovx_rad / 2) / aspect)
    fovy_deg = math.degrees(fovy_rad)
    print(f"FOV: Cesium horizontal {fovx_deg:.2f}° -> Filament vertical {fovy_deg:.2f}°", flush=True)

    ours_png = os.path.join(args.out, 'ours.png')
    cmd = [
        'xvfb-run', '-a',
        args.demo,
        '--tileset', args.tileset,
        '--width', str(params['width']),
        '--height', str(params['height']),
        '--camera-eye', f"{eye[0]},{eye[1]},{eye[2]}",
        '--camera-target', f"{target[0]},{target[1]},{target[2]}",
        '--camera-up', f"{up[0]},{up[1]},{up[2]}",
        '--fov', str(fovy_deg),
        '--near', str(cam['near']),
        '--far', str(cam['far']),
        '--background', f"{bg[0]},{bg[1]},{bg[2]},{bg[3]}",
        '--no-ibl',  # benchmark disables IBL
        '--frames', str(args.frames),
        '--settle-before-screenshot', '30',
        '--screenshot', ours_png,
    ]

    print(f"Running: {' '.join(cmd[:4])} ...", flush=True)
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        print(f"Demo failed (rc={result.returncode})", file=sys.stderr)
        print(result.stderr[-2000:], file=sys.stderr)
        sys.exit(1)

    if not os.path.exists(ours_png):
        print(f"Screenshot not created: {ours_png}", file=sys.stderr)
        sys.exit(1)

    # Compare.
    cesium_png = os.path.join(args.benchmark, 'render.png')
    diff_png = os.path.join(args.out, 'diff.png')
    compare_json = os.path.join(args.out, 'compare.json')

    cmp_cmd = [
        sys.executable,
        os.path.join(SCRIPT_DIR, 'image_compare.py'),
        '--a', ours_png,
        '--b', cesium_png,
        '--out', diff_png,
        '--json',
    ]
    result = subprocess.run(cmp_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Compare failed: {result.stderr}", file=sys.stderr)
        sys.exit(1)

    with open(compare_json, 'w') as f:
        f.write(result.stdout)

    compare = json.loads(result.stdout)
    print(f"SSIM={compare['ssim']:.4f} NCC={compare['ncc']:.4f} "
          f"pixels_differ={compare['pct_pixels_differ']:.1f}%")
    print(f"Output: {args.out}")


if __name__ == '__main__':
    main()
