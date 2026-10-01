#!/usr/bin/env python3
"""
P37 Step 2: Image comparison tool.

Compares two PNG images and outputs:
  - SSIM (structural similarity, 0-1, higher is more similar)
  - NCC (normalized cross-correlation, -1 to 1, higher is more similar)
  - Pixel stats (mean/max abs diff, % pixels above threshold)
  - Diff heatmap PNG (visual localization of differences)

Usage:
  python3 image_compare.py --a <png> --b <png> [--out <diff.png>] [--json]

Exit codes: 0 = compared OK, 2 = error (size mismatch, file not found).
Note: PASS/FAIL threshold is decided by human review, not hardcoded here.
"""
import argparse
import json
import sys

try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("Need PIL and numpy", file=sys.stderr)
    sys.exit(2)


def compute_ssim(img1, img2, window_size=11):
    """Mean SSIM over grayscale images (0-1 range)."""
    K1, K2, L = 0.01, 0.03, 1.0
    C1, C2 = (K1 * L) ** 2, (K2 * L) ** 2

    if len(img1.shape) == 3:
        img1 = img1.mean(axis=2)
    if len(img2.shape) == 3:
        img2 = img2.mean(axis=2)

    try:
        from scipy.ndimage import uniform_filter
        def blur(img):
            return uniform_filter(img, size=window_size, mode='reflect')
    except ImportError:
        # Fallback: uniform box blur via cumsum.
        def blur(img):
            h, w = img.shape
            pad = window_size // 2
            p = np.pad(img, pad, mode='reflect')
            # Integral image.
            ii = np.cumsum(np.cumsum(p, axis=0), axis=1)
            ii = np.pad(ii, ((1, 0), (1, 0)))
            ws = window_size
            out = (ii[ws:, ws:] - ii[:-ws, ws:] - ii[ws:, :-ws] + ii[:-ws, :-ws]) / (ws * ws)
            return out

    mu1, mu2 = blur(img1), blur(img2)
    mu1_sq, mu2_sq, mu1_mu2 = mu1 ** 2, mu2 ** 2, mu1 * mu2
    sigma1_sq = blur(img1 ** 2) - mu1_sq
    sigma2_sq = blur(img2 ** 2) - mu2_sq
    sigma12 = blur(img1 * img2) - mu1_mu2

    ssim_map = ((2 * mu1_mu2 + C1) * (2 * sigma12 + C2)) / \
               ((mu1_sq + mu2_sq + C1) * (sigma1_sq + sigma2_sq + C2))
    return float(ssim_map.mean())


def compute_ncc(img1, img2):
    """Normalized cross-correlation over all channels (-1 to 1)."""
    a = img1.flatten().astype(np.float64)
    b = img2.flatten().astype(np.float64)
    a = a - a.mean()
    b = b - b.mean()
    denom = np.sqrt((a ** 2).sum() * (b ** 2).sum())
    if denom == 0:
        return 1.0 if np.allclose(a, b) else 0.0
    return float((a * b).sum() / denom)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--a', required=True, help='First PNG')
    ap.add_argument('--b', required=True, help='Second PNG')
    ap.add_argument('--out', default=None, help='Diff heatmap output PNG')
    ap.add_argument('--json', action='store_true', help='Output JSON')
    args = ap.parse_args()

    try:
        img_a = np.array(Image.open(args.a).convert('RGB'), dtype=np.float64) / 255.0
        img_b = np.array(Image.open(args.b).convert('RGB'), dtype=np.float64) / 255.0
    except FileNotFoundError as e:
        print(f"File not found: {e}", file=sys.stderr)
        sys.exit(2)

    if img_a.shape != img_b.shape:
        print(f"Size mismatch: {img_a.shape} vs {img_b.shape}", file=sys.stderr)
        sys.exit(2)

    ssim_score = compute_ssim(img_a, img_b)
    ncc_score = compute_ncc(img_a, img_b)

    abs_diff = np.abs(img_a - img_b)
    mean_diff = float(abs_diff.mean())
    max_diff = float(abs_diff.max())
    # % of pixels where any channel differs by more than 1/255.
    diff_mask = (abs_diff > 1.0 / 255.0).any(axis=2)
    pct_diff = float(diff_mask.mean() * 100)

    result = {
        'ssim': round(ssim_score, 4),
        'ncc': round(ncc_score, 4),
        'mean_abs_diff': round(mean_diff, 4),
        'max_abs_diff': round(max_diff, 4),
        'pct_pixels_differ': round(pct_diff, 2),
        'width': img_a.shape[1],
        'height': img_a.shape[0],
    }

    if args.out:
        # Diff heatmap: red = large diff, black = no diff.
        heat = (abs_diff.mean(axis=2) * 255).clip(0, 255).astype(np.uint8)
        heat_img = Image.fromarray(heat, mode='L')
        # Colorize: map to red channel.
        red = Image.new('L', heat_img.size, 0)
        merged = Image.merge('RGB', (heat_img, red, red))
        merged.save(args.out)
        result['diff_heatmap'] = args.out

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"SSIM={result['ssim']:.4f} NCC={result['ncc']:.4f} "
              f"mean_diff={result['mean_abs_diff']:.4f} max_diff={result['max_abs_diff']:.4f} "
              f"pixels_differ={result['pct_pixels_differ']:.1f}%")

    sys.exit(0)


if __name__ == '__main__':
    main()
