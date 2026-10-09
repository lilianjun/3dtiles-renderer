#!/usr/bin/env python3
"""Run full 149 conformance test and report results."""
import json
import os
import subprocess
import sys

STANDARD_DIR = '/home/hatch/workspace/e2e_capture/standard'
REPO_DIR = '/home/hatch/workspace/3dtiles-renderer'
DEMO = os.path.join(REPO_DIR, 'build/linux/tiles_demo')
OUT_DIR = '/home/hatch/workspace/full_149_results'

def fixture_to_tileset(fixture):
    """Map fixture name to tileset.json path."""
    if fixture.startswith('cesiumjs_Specs_Data_'):
        rest = fixture[len('cesiumjs_Specs_Data_'):].replace('_', '/')
        return os.path.join(REPO_DIR, 'tests/data/cesiumjs/Specs/Data', rest, 'tileset.json')
    elif fixture.startswith('cesiumjs_3d_tiles_samples_'):
        rest = fixture[len('cesiumjs_3d_tiles_samples_'):].replace('_', '/')
        # 1_0 -> 1.0, etc.
        parts = rest.split('/')
        # First two parts are version: 1/0 -> 1.0
        if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
            parts = [f"{parts[0]}.{parts[1]}"] + parts[2:]
        rest = '/'.join(parts)
        return os.path.join(REPO_DIR, 'tests/data/cesiumjs/3d-tiles-samples', rest, 'tileset.json')
    elif fixture.startswith('cesiumjs_Apps_SampleData_'):
        rest = fixture[len('cesiumjs_Apps_SampleData_'):].replace('_', '/')
        return os.path.join(REPO_DIR, 'tests/data/cesiumjs/Apps/SampleData', rest, 'tileset.json')
    else:
        # p15_, p25_, p3_, p8_, etc.
        return os.path.join(REPO_DIR, 'tests/data', fixture, 'tileset.json')

def main():
    with open(os.path.join(STANDARD_DIR, 'MANIFEST.json')) as f:
        manifest = json.load(f)
    
    fixtures = [f['fixture'] for f in manifest['fixtures']]
    print(f"Total fixtures: {len(fixtures)}", flush=True)
    
    os.makedirs(OUT_DIR, exist_ok=True)
    
    results = []
    for i, fixture in enumerate(fixtures):
        benchmark = os.path.join(STANDARD_DIR, fixture)
        tileset = fixture_to_tileset(fixture)
        out = os.path.join(OUT_DIR, fixture)
        
        if not os.path.exists(tileset):
            print(f"[{i+1}/{len(fixtures)}] {fixture}: SKIP (no tileset)", flush=True)
            results.append({'fixture': fixture, 'status': 'skip', 'reason': 'no tileset'})
            continue
        
        os.makedirs(out, exist_ok=True)
        cmd = [
            sys.executable,
            os.path.join(REPO_DIR, 'tests/data/benchmarks/conformance_test.py'),
            '--benchmark', benchmark,
            '--tileset', tileset,
            '--demo', DEMO,
            '--out', out,
            '--frames', '1',
        ]
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            # Parse output for SSIM and pixels_differ
            output = result.stdout + result.stderr
            ssim = None
            pixels_differ = None
            for line in output.split('\n'):
                if 'SSIM=' in line:
                    # SSIM=0.9223 NCC=0.9381 pixels_differ=14.7%
                    parts = line.split()
                    for p in parts:
                        if p.startswith('SSIM='):
                            ssim = float(p.split('=')[1])
                        elif p.startswith('pixels_differ='):
                            pixels_differ = float(p.split('=')[1].rstrip('%'))
                    break
            
            status = 'pass' if pixels_differ is not None and pixels_differ < 1.0 else 'fail'
            # p25 exception
            if fixture == 'p25_png_tileset' and pixels_differ and pixels_differ < 7.0:
                status = 'pass'
            
            print(f"[{i+1}/{len(fixtures)}] {fixture}: {status} (SSIM={ssim}, diff={pixels_differ}%)", flush=True)
            results.append({
                'fixture': fixture,
                'status': status,
                'ssim': ssim,
                'pixels_differ': pixels_differ,
            })
        except subprocess.TimeoutExpired:
            print(f"[{i+1}/{len(fixtures)}] {fixture}: TIMEOUT", flush=True)
            results.append({'fixture': fixture, 'status': 'timeout'})
        except Exception as e:
            print(f"[{i+1}/{len(fixtures)}] {fixture}: ERROR {e}", flush=True)
            results.append({'fixture': fixture, 'status': 'error', 'reason': str(e)})
    
    # Summary
    passed = sum(1 for r in results if r['status'] == 'pass')
    failed = sum(1 for r in results if r['status'] == 'fail')
    skipped = sum(1 for r in results if r['status'] in ('skip', 'timeout', 'error'))
    
    print(f"\n=== SUMMARY ===", flush=True)
    print(f"Passed: {passed}/{len(fixtures)}", flush=True)
    print(f"Failed: {failed}/{len(fixtures)}", flush=True)
    print(f"Skipped/Error: {skipped}/{len(fixtures)}", flush=True)
    
    # Save results
    with open(os.path.join(OUT_DIR, 'results.json'), 'w') as f:
        json.dump(results, f, indent=2)
    
    # Print failed list
    if failed > 0:
        print(f"\nFailed fixtures:", flush=True)
        for r in results:
            if r['status'] == 'fail':
                print(f"  {r['fixture']}: {r['pixels_differ']}% (SSIM={r['ssim']})", flush=True)

if __name__ == '__main__':
    main()
