#!/usr/bin/env python3
"""
P37-C2: SSIM comparison between our render and cesium.js render.

Usage:
    python3 compare_ssim.py --ours <png> --cesium <png> [--threshold 0.95]

Outputs SSIM score and PASS/FAIL against threshold.
"""
import argparse
import sys

try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("Need PIL and numpy: pip install pillow numpy", file=sys.stderr)
    sys.exit(2)

def ssim(img1, img2, window_size=11):
    """Compute mean SSIM between two grayscale images (0-1 range)."""
    # Simplified SSIM (no Gaussian weighting, uses uniform window).
    # For conformance testing, this is sufficient.
    K1, K2, L = 0.01, 0.03, 1.0
    C1, C2 = (K1 * L) ** 2, (K2 * L) ** 2

    # Convert to float grayscale.
    if len(img1.shape) == 3:
        img1 = img1.mean(axis=2)
    if len(img2.shape) == 3:
        img2 = img2.mean(axis=2)

    # Pad for window.
    pad = window_size // 2
    img1_p = np.pad(img1, pad, mode='reflect')
    img2_p = np.pad(img2, pad, mode='reflect')

    # Uniform filter via cumsum (fast box blur).
    def box_blur(img, w):
        # Integral image approach.
        cumsum = np.cumsum(np.cumsum(img, axis=0), axis=1)
        # Use slicing for box filter.
        h, ww = img.shape
        # Simplified: use uniform_filter from scipy if available, else naive.
        try:
            from scipy.ndimage import uniform_filter
            return uniform_filter(img, size=w, mode='reflect')
        except ImportError:
            # Naive (slow but works for small images).
            out = np.zeros((h - w + 1, ww - w + 1))
            for i in range(out.shape[0]):
                for j in range(out.shape[1]):
                    out[i, j] = img[i:i+w, j:j+w].mean()
            return out

    mu1 = box_blur(img1_p, window_size)
    mu2 = box_blur(img2_p, window_size)
    mu1_sq, mu2_sq = mu1 ** 2, mu2 ** 2
    mu1_mu2 = mu1 * mu2

    sigma1_sq = box_blur(img1_p ** 2, window_size) - mu1_sq
    sigma2_sq = box_blur(img2_p ** 2, window_size) - mu2_sq
    sigma12 = box_blur(img1_p * img2_p, window_size) - mu1_mu2

    ssim_map = ((2 * mu1_mu2 + C1) * (2 * sigma12 + C2)) / \
               ((mu1_sq + mu2_sq + C1) * (sigma1_sq + sigma2_sq + C2))
    return float(ssim_map.mean())

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ours', required=True, help='Our render PNG')
    ap.add_argument('--cesium', required=True, help='Cesium.js render PNG')
    ap.add_argument('--threshold', type=float, default=0.95)
    args = ap.parse_args()

    img1 = np.array(Image.open(args.ours).convert('RGB'), dtype=np.float64) / 255.0
    img2 = np.array(Image.open(args.cesium).convert('RGB'), dtype=np.float64) / 255.0

    if img1.shape != img2.shape:
        print(f"Size mismatch: {img1.shape} vs {img2.shape}", file=sys.stderr)
        sys.exit(2)

    score = ssim(img1, img2)
    status = "PASS" if score >= args.threshold else "FAIL"
    print(f"SSIM={score:.4f} threshold={args.threshold} {status}")
    sys.exit(0 if status == "PASS" else 1)

if __name__ == '__main__':
    main()
