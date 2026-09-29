# ADR-0024: KTX2 (KHR_texture_basisu) texture support (P25)

## Status

Accepted (2026-09-29). KTX2/BasisU textures render correctly on Linux;
other platforms use Filament's provider (no cesium there, hence no
conflict — see below). Verified by `tests/ktx2_test.py` (CTest `ktx2`
+ `sanitizer_ktx2`).

## Context

P15 wired gltfio's stb provider for PNG/JPEG but left KTX2 (`image/ktx2`,
`KHR_texture_basisu`) unregistered — such textures rendered black with a
"Missing texture provider" log. P25 set out to truly verify and support
KTX2 with a self-generated deterministic fixture.

## What we found (all empirically verified, 2026-09-29)

1. **Uncompressed KTX2 can never work through this pipeline.**
   cesium-native v0.64.0's `ImageDecoder` (`CesiumImage/src/ImageDecoder.cpp`)
   only returns success for KTX2 when `ktxTexture2_NeedsTranscoding()` is
   true; uncompressed KTX2 falls through to an error branch that reports
   `KTX2 loading failed with error: Operation succeeded.` The tile then
   fails (0 tiles rendered). A hand-written, `ktx2check`-clean uncompressed
   RGBA8 KTX2 fixture confirmed this. So the positive fixture must be
   BasisU-compressed (ETC1S/UASTC).

2. **Filament's `createKtx2Provider` is correct — the crash was a symbol
   collision, not a Filament bug.** A standalone probe linking ONLY
   Filament's `libbasis_transcoder.a` transcoded our UASTC fixture
   perfectly (all mip levels, RGBA32 and ETC2_RGBA paths, correct pixels).
   But in the demo it segfaulted on shutdown (release) and OOM'd inside
   `transcodeImageLevel` under ASan. The ASan stack trace showed the
   executing code was
   `~/.ezvcpkg/.../buildtrees/ktx/.../external/basisu/transcoder/basisu_transcoder.cpp`
   — i.e. **cesium-native's vcpkg `libktx.a` vendors its own `basist::`
   basisu copy** (107 symbols), a different version with a different class
   layout than Filament's. The static linker resolved Filament's ktxreader
   references against cesium's incompatible basisu → heap corruption.
   Decisive proof: the same probe binary segfaults (exit 139) when
   `libktx.a` is linked before `libbasis_transcoder.a`, and works when it
   is not. After the fix, `nm` on the demo shows zero duplicate `basist::`
   symbols.

3. **The fix: one basisu per binary.** On cesium builds (Linux) we do NOT
   link `ktxreader`/`basis_transcoder` at all. Instead
   `src/ktx2_libktx_provider.cpp` implements `gltfio::TextureProvider`
   directly on top of the **same `libktx` cesium-native already links**
   (`ktxTexture2_CreateFromMemory` + `ktxTexture2_TranscodeBasis` to
   `KTX_TTF_RGBA32`, the same fallback cesium's own `ImageDecoder` uses),
   uploading as RGBA8/SRGB8_A8 with mipmap generation. Synchronous
   (gltfio calls providers on the foreground thread); corrupt input
   returns `nullptr` with a message and the demo exits 0.
   On non-cesium platforms (Windows/Android/iOS: no `libktx`, hence no
   conflict) `src/tileset.cpp` uses Filament's `createKtx2Provider`
   (`TILES_WITH_KTX2_PROVIDER`); the transcoder itself was proven correct
   by the standalone probe. WASM has no Filament and no KTX2 support.

## Decision

- `TILES_WITH_KTX2_LIBKTX` (cesium builds): custom libktx-backed provider,
  `src/ktx2_libktx_provider.cpp`, `ktx.h` from cesium's vcpkg tree.
- `TILES_WITH_KTX2_PROVIDER` (filament builds without cesium): Filament's
  `createKtx2Provider`, archives existence-checked like P15's stb.
- Fixture: `tests/data/gen_p25_ktx2.py` embeds a fixed UASTC payload
  (base64; produced once by Khronos `toktx 4.4.2 --uastc`, 64x64 red/blue
  checkerboard, 1 mip, sRGB). The UASTC decode is lossless for this
  flat-color image (0/4096 texels differ from the PNG), so the test
  asserts a tight KTX2-vs-PNG tolerance (max ≤ 16, mean < 2.0; measured
  0.0/0.0000). Determinism verified (re-run → identical md5).
- Corrupt KTX2 (zeroed magic, truncated file) fails gracefully: load
  error logged, demo exits 0, 0 tiles rendered — no crash, no hang.

## Consequences / honest boundaries

- **Transcode target is RGBA32, not a GPU block format.** We trade GPU
  memory for correctness and simplicity; the BasisU size advantage is
  future work. Recorded here so nobody mistakes "KTX2 supported" for
  "GPU-compressed textures".
- **Only Linux is pixel-verified** (Mesa/xvfb, like everything else).
  Windows/Android/iOS take Filament's provider, which is vendor code with
  a proven-correct transcoder and no symbol conflict on those builds —
  but no pixels have been verified there (same standing boundary as
  G1 in ADR-0023).
- The `basist::` collision is a latent hazard for any future dependency
  that vendors basisu: if it reappears, `nm <binary> | grep _ZN6basist`
  with `sort | uniq -d` must show zero duplicates.
- No public API changes (internal provider only).
