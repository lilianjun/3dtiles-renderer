# ADR-0025: Image-based lighting (P26)

## Status

Accepted (2026-09-29). Default-on procedural IBL in the SDK; verified by
`tests/ibl_test.py` (CTest `ibl` + `sanitizer_ibl`) on Linux/Mesa. Goldens
re-frozen for the intentional lighting change (9/11 entries).

## Context

Since P2/P3 the scene's only light has been the P3 directional sun
(100,000 lux). ADR-0013 documented the consequence: metals can only show
the sun's specular lobe and are otherwise black, because nothing else in
the scene emits light. Measured 2026-09-29 on the `p15_metal` fixture
(Mesa, 800x600): metallic box mean luminance 5.6 (max 30), roughness-0.08
metal exactly 0.0. Any glTF asset with metalness > 0 looked wrong.

## Decision

Add a Filament `IndirectLight` driven by a small **procedural**
environment, generated deterministically at `Renderer::initialize()`:

- **Reflections:** 6 cubemap faces, 64x64 RGBA8, filled from an analytic
  outdoor function (sky horizon→zenith gradient, dark ground, sun disc +
  glow placed at the direction *toward* the sun, matching the P3
  directional light). Mipmaps via `Texture::generateMipmaps`
  (`DEFAULT | GEN_MIPMAPPABLE` usage).
- **Diffuse irradiance:** 3-band spherical harmonics of the *same*
  function, projected by deterministic Fibonacci-sphere quadrature
  (2048 samples), passed via `IndirectLight::Builder::radiance(3, sh)`.
  Verified against Filament's header convention
  (`index(l,m) = l*(l+1)+m`, no Condon-Shortley phase) by rendering: the
  sky-bright direction lights upward faces, as expected.
- **Intensity:** Filament default (30000); the environment is 0..1 LDR,
  so no HDR range is needed for a plausible outdoor ambient.

Why procedural instead of an embedded `.ktx`: zero new dependencies, zero
binary blobs, bit-deterministic, and identical on every platform — it uses
only Filament core API (`Texture`, `IndirectLight`, `Scene`), so the
Win/Android/iOS code path compiles unchanged. (WASM stub: no-op, as with
all Filament paths.)

Public API is minimal and documented in `renderer.h`:

```cpp
static void setIblEnabled(bool enabled); // default on; false = sun only
```

On by default; `setIblEnabled(false)` detaches the `IndirectLight` from the
scene (the cubemap/SH survive the session, so toggling is cheap). The demo
exposes `--no-ibl` for A/B pixel tests. No brightness knob in this
version — deliberately, to keep the API surface minimal.

## Measured effect (2026-09-29, Mesa+xvfb, 800x600, 60 frames)

| fixture region | IBL off (pre-P26) | IBL on |
|---|---|---|
| metal box (metallic=1, rough~0.3) mean/max | 5.6 / 30 | 55.2 / 88 |
| smooth metal (rough 0.08) mean/max | 0.0 / 0 | 57.7 / 75 |
| rough metal (rough 0.9) mean/max | 18.6 / 95 | 56.4 / 127 |
| dielectric box mean/max | 15.1 / 112 | 58.9 / 152 |

The metal is ~10x brighter with clear sky-reflection character
(mean color shifts from `[5.5, 5.4, 5.8]` to `[42, 50, 73]`); the mirror
metal is no longer black; nothing blows out (max 152 < 220).

## Consequences

- **Goldens re-frozen (9/11):** every lit golden changed (IBL adds
  ambient everywhere); `p2_demo` and `p8_pnts` (unlit) are unchanged.
  Each changed image was human-reviewed: geometry pixel counts identical,
  only lighting changed (e.g. `p3_tileset`: same 275670 box pixels, mean
  13.2 → 62.9). Regenerated via `tests/golden/regenerate.py --confirm`
  per the policy in `tests/golden/README.md`.
- **`tests/pbr_test.py`:** the `metal` and `rough` checks now render with
  `--no-ibl` — their assertions are statements about the PBR material
  model under the directional sun ("metal has no diffuse", "smooth metal
  is ~black"), which the default IBL would mask. The other four checks
  (normal/alpha/sided/tex) run under default IBL and still pass.
- **Tradeoff (honest):** the reflection mips are plain box-filtered GPU
  mips, not cmgen-quality GGX prefiltering — plausible for a default
  outdoor environment, not reference quality. Transcode is to RGBA32 LDR;
  no HDR environment. A future phase may add cmgen-prefiltered or HDR
  environments and an intensity knob.

## Verification

- `tests/ibl_test.py`: on/off A/B on `p15_metal`; asserts metal mean >
  3x off-mean, color shift toward sky (blue-dominant), dielectric sane
  (brighter, max < 220, top face lit), identical geometry pixel counts.
- `sanitizer_ibl`: ASan/LSan/UBSan lane over the IBL-on render (cubemap
  uploads, SH, `IndirectLight`, mipmap generation).
- Full suite: CTest 26/26 on Linux; SDK `nm` shows 0 SDL symbols;
  `git diff --check` clean.
