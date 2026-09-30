#!/usr/bin/env python3
"""P15: PBR material pipeline verification.

Renders the deterministic fixtures from data/gen_p15_pbr_tileset.py through
tiles_demo and asserts that the glTF PBR material model is really driving
pixels (via gltfio's ubershader + the scene's directional sun):

  metal  - metallic=0 vs metallic=1 (same base color/roughness): the metal
           box must be dramatically darker (no diffuse under no-IBL) and the
           two regions must differ significantly. Rendered with --no-ibl:
           the assertion is about the PBR model under the directional sun;
           the default IBL would mask it with ambient (see ibl_test.py).
  rough  - metallic boxes, roughness=0.08 vs 0.9: the mirror-smooth metal is
           ~black (nothing to reflect), the rough metal shows a broad
           specular sheen on its top face. Rendered with --no-ibl for the
           same reason (with IBL on, the mirror reflects the environment).
  normal - hand-made normal map vs flat normals: shading must differ.
  alpha  - same red box OPAQUE vs BLEND(0.5): the blend render must differ
           strongly, its lit top face must darken toward the interior color
           (between-ness), and interior faces must become visible through
           the transparent shell.
  sided  - plane facing away from the camera: single-sided fully culled,
           double-sided visible.
  tex    - procedural checkerboard baseColor texture: both tones sampled.

No hard-coded pixel values: every assertion is a comparison between regions
or renders, or a "within a sane range" check, so Mesa's exact shading
numbers are not baked in.

Usage:
  pbr_test.py --demo <tiles_demo> --outdir <dir> [--frames 60 ...]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")

from PIL import Image  # noqa: E402
import numpy as np  # noqa: E402

BG = np.array([26, 51, 115])  # demo clear color
LX, RX = 340, 460  # left/right region split (dead zone in the middle)


def render_demo(demo, tileset, out, frames, width, height, no_ibl=False):
    # P30: settle-gated capture (kills the fixed-frame screenshot race).
    # `frames` is the give-up budget.
    cmd = [demo, "--until-loaded", str(4 * frames),
           "--width", str(width), "--height", str(height),
           "--tileset", tileset, "--screenshot", out]
    if no_ibl:
        # P26: pin the pre-P26 scene (directional sun only) for the checks
        # whose assertions are no-IBL statements about the PBR model.
        cmd.append("--no-ibl")
    if not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        return None, "demo exited %d" % proc.returncode
    rendered = -1
    for line in proc.stdout.splitlines():
        if "tiles rendered (last frame):" in line:
            try:
                rendered = int(line.rsplit(":", 1)[1].strip())
            except ValueError:
                pass
    if not os.path.exists(out):
        return None, "screenshot not written: %s" % out
    img = np.asarray(Image.open(out).convert("RGB")).astype(np.int32)
    if img.shape[1] != width or img.shape[0] != height:
        return None, "screenshot size %dx%d, expected %dx%d" % (
            img.shape[1], img.shape[0], width, height)
    return (rendered, img), None


def nonbg(img):
    return np.sqrt(((img - BG) ** 2).sum(axis=2)) > 30


class Region:
    def __init__(self, img, x0, x1):
        self.mask = nonbg(img)[:, x0:x1]
        self.px = img[:, x0:x1][self.mask]
        self.n = int(self.mask.sum())
        lum = self.px.mean(axis=1) if self.n else np.array([0.0])
        self.mean_lum = float(lum.mean())
        self.max_lum = float(lum.max())
        self.mean_color = self.px.mean(axis=0) if self.n else np.zeros(3)


def color_dist(a, b):
    return float(np.sqrt(((a - b) ** 2).sum()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--frames", type=int, default=60,
                      help="P30: max frames (settle budget)")
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    failures = []

    def render(name, tileset, no_ibl=False):
        out = os.path.join(args.outdir, "p15_%s.png" % name)
        (res, err) = render_demo(args.demo, tileset, out,
                                 args.frames, args.width, args.height,
                                 no_ibl=no_ibl)
        if err is not None:
            failures.append("%s: %s" % (name, err))
            return None, None
        return res

    # --- metal: dielectric vs metal ---
    # P26: rendered with --no-ibl. This check's assertions are statements
    # about the PBR material model under the directional sun ("Metal has no
    # diffuse"), which the default IBL would mask with ambient; the IBL
    # on/off behavior itself is covered by ibl_test.py.
    print("=== metal: metallic=0 vs metallic=1 (no IBL) ===", flush=True)
    rendered, img = render("metal", os.path.join(DATA, "p15_metal",
                                                 "tileset.json"),
                           no_ibl=True)
    if img is not None:
        if rendered != 2:
            failures.append("metal: expected 2 tiles, got %d" % rendered)
        left, right = Region(img, 0, LX), Region(img, RX, 800)
        print("diel n=%d mean=%.1f max=%.0f | metal n=%d mean=%.1f max=%.0f"
              % (left.n, left.mean_lum, left.max_lum,
                 right.n, right.mean_lum, right.max_lum), flush=True)
        if left.n < 5000 or right.n < 5000:
            failures.append("metal: a box is missing (n=%d/%d)"
                            % (left.n, right.n))
        # Metal has no diffuse: under the sun with no IBL it must be much
        # darker than the dielectric with the same base color.
        if not right.mean_lum < 0.6 * left.mean_lum:
            failures.append("metal: metal not darker than dielectric "
                            "(%.1f vs %.1f)" % (right.mean_lum, left.mean_lum))
        if color_dist(left.mean_color, right.mean_color) < 8.0:
            failures.append("metal: regions too similar (dist=%.1f)"
                            % color_dist(left.mean_color, right.mean_color))
        if left.max_lum < 80:
            failures.append("metal: dielectric top face not lit (max=%.0f)"
                            % left.max_lum)

    # --- rough: metal, roughness 0.08 vs 0.9 ---
    # P26: rendered with --no-ibl, like the metal check: the assertions are
    # statements about the specular lobe under the directional sun, and the
    # "smooth metal is ~black" assertion only holds without an environment
    # to reflect (with IBL on, the mirror shows the sky — see ibl_test.py).
    print("=== rough: roughness=0.08 vs 0.9 (metal, no IBL) ===", flush=True)
    rendered, img = render("rough", os.path.join(DATA, "p15_rough",
                                                 "tileset.json"),
                           no_ibl=True)
    if img is not None:
        if rendered != 2:
            failures.append("rough: expected 2 tiles, got %d" % rendered)
        smooth, rough = Region(img, 0, LX), Region(img, RX, 800)
        print("r0.08 n=%d mean=%.1f max=%.0f | r0.9 n=%d mean=%.1f max=%.0f"
              % (smooth.n, smooth.mean_lum, smooth.max_lum,
                 rough.n, rough.mean_lum, rough.max_lum), flush=True)
        if smooth.n < 5000 or rough.n < 5000:
            failures.append("rough: a box is missing (n=%d/%d)"
                            % (smooth.n, rough.n))
        # Mirror-smooth metal reflects the (empty) environment -> ~black;
        # rough metal scatters the sun into a broad visible sheen.
        if not (rough.mean_lum > 10.0 and smooth.mean_lum < 5.0):
            failures.append("rough: sheen behavior wrong (smooth=%.1f, "
                            "rough=%.1f)" % (smooth.mean_lum, rough.mean_lum))
        if not (rough.max_lum > 50.0 and smooth.max_lum < 20.0):
            failures.append("rough: highlight range wrong (smooth max=%.0f, "
                            "rough max=%.0f)" % (smooth.max_lum,
                                                 rough.max_lum))

    # --- normal: normal-mapped vs flat ---
    print("=== normal: normal map vs flat ===", flush=True)
    rendered, img = render("normal", os.path.join(DATA, "p15_normal",
                                                  "tileset.json"))
    if img is not None:
        if rendered != 2:
            failures.append("normal: expected 2 tiles, got %d" % rendered)
        nmap, flat = Region(img, 0, LX), Region(img, RX, 800)
        print("nmap n=%d mean=%.1f max=%.0f | flat n=%d mean=%.1f max=%.0f"
              % (nmap.n, nmap.mean_lum, nmap.max_lum,
                 flat.n, flat.mean_lum, flat.max_lum), flush=True)
        if nmap.n < 5000 or flat.n < 5000:
            failures.append("normal: a box is missing (n=%d/%d)"
                            % (nmap.n, flat.n))
        # The normal map must perturb shading: region colors and peak
        # brightness must differ from the flat twin.
        if color_dist(nmap.mean_color, flat.mean_color) < 3.0:
            failures.append("normal: normal map had no effect (dist=%.1f)"
                            % color_dist(nmap.mean_color, flat.mean_color))
        if abs(nmap.max_lum - flat.max_lum) < 8.0:
            failures.append("normal: highlight peaks identical (%.0f/%.0f)"
                            % (nmap.max_lum, flat.max_lum))

    # --- alpha: OPAQUE vs BLEND renders of the same red box ---
    print("=== alpha: OPAQUE vs BLEND ===", flush=True)
    r_o, img_o = render("alpha_opaque", os.path.join(
        DATA, "p15_alpha_opaque", "tileset.json"))
    r_b, img_b = render("alpha_blend", os.path.join(
        DATA, "p15_alpha_blend", "tileset.json"))
    if img_o is not None and img_b is not None:
        if r_o != 1 or r_b != 1:
            failures.append("alpha: expected 1 tile each, got %d/%d"
                            % (r_o, r_b))
        diff = (np.abs(img_o - img_b).sum(axis=2) > 30).sum()
        print("full-image differing px: %d" % diff, flush=True)
        if diff < 5000:
            failures.append("alpha: BLEND barely changed the image "
                            "(diff px=%d)" % diff)
        # Lit red top face in the opaque render; in the blend render the
        # same pixels must sit between the opaque red and the dark
        # interior (between-ness).
        top = ((img_o[:, :, 0] > 100) & (img_o[:, :, 1] < 80)
               & (img_o[:, :, 2] < 80))
        if top.sum() < 500:
            failures.append("alpha: no lit top face found in opaque render")
        else:
            ro = img_o[top][:, 0].mean()
            rb = img_b[top][:, 0].mean()
            print("top face red: opaque=%.1f blend=%.1f" % (ro, rb),
                  flush=True)
            if not (0.3 * ro < rb < 0.8 * ro):
                failures.append("alpha: blend top not between opaque red "
                                "and interior (%.1f vs %.1f)" % (rb, ro))
        # Interior faces become visible through the transparent shell.
        # P26: the old dim-red pixel count broke under IBL (interiors are
        # now ambient-lit, brighter and bluer). This metric measures the
        # transparency mechanics instead, which are lighting-independent:
        # inside the box silhouette, the blend render must differ strongly
        # from the opaque render over a large pixel set. Verified identical
        # (10292 px) with IBL on and off; a BLEND-rendered-as-opaque
        # failure would give ~0.
        diff_map = np.sqrt(((img_b - img_o) ** 2).sum(axis=2))
        silhouette = nonbg(img_o) | nonbg(img_b)
        far_bg = np.sqrt(((img_b - BG) ** 2).sum(axis=2)) > 30
        interior_px = int(((diff_map > 40) & silhouette & far_bg).sum())
        print("interior revealed px: %d" % interior_px, flush=True)
        if not interior_px > 5000:
            failures.append("alpha: interior not revealed by BLEND "
                            "(%d px)" % interior_px)

    # --- sided: plane facing away from the camera ---
    print("=== sided: single vs double ===", flush=True)
    r_s, img_s = render("sided_single", os.path.join(
        DATA, "p15_sided_single", "tileset.json"))
    r_d, img_d = render("sided_double", os.path.join(
        DATA, "p15_sided_double", "tileset.json"))
    if img_s is not None and img_d is not None:
        ns, nd = nonbg(img_s).sum(), nonbg(img_d).sum()
        print("non-bg px: single=%d double=%d" % (ns, nd), flush=True)
        if ns > 100:
            failures.append("sided: single-sided plane not culled "
                            "(non-bg px=%d)" % ns)
        if nd < 2000:
            failures.append("sided: double-sided plane invisible "
                            "(non-bg px=%d)" % nd)

    # --- tex: checkerboard baseColor texture ---
    print("=== tex: checkerboard baseColor ===", flush=True)
    rendered, img = render("tex", os.path.join(DATA, "p15_tex",
                                               "tileset.json"))
    if img is not None:
        if rendered != 1:
            failures.append("tex: expected 1 tile, got %d" % rendered)
        reg = Region(img, 0, 800)
        bright = int(((reg.px > 150).all(axis=1)).sum())
        dark = int(((reg.px < 100).all(axis=1)).sum())
        print("box px=%d bright=%d dark=%d" % (reg.n, bright, dark),
              flush=True)
        if reg.n < 5000:
            failures.append("tex: box missing (n=%d)" % reg.n)
        if bright < 500 or dark < 5000:
            failures.append("tex: checkerboard not sampled "
                            "(bright=%d dark=%d)" % (bright, dark))

    print("pbr: %s" % ("FAIL" if failures else "ok"), flush=True)
    for f in failures:
        print("FAIL:", f, flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
