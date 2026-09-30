# ADR-0029: Settle-gated screenshots (kills the fixed-frame flake class)

## Status

Accepted (2026-09-30).

## Context

P26, P27 and P28 each saw one flaky screenshot-test failure with the same
signature: a fixed-frame render (`--frames 60`) screenshotted a tileset
whose async loading had not yet converged (113 px off in `p7_i3dm`,
`tileset_switch` S3 md5 mismatch). Re-running always passed. This was not
a product bug — it was the test harness racing cesium-native's async
tile pipeline.

## Decision

Screenshots are now taken only after the tileset demonstrably converges,
via two demo-side mechanisms (both test-only; zero SDK changes, zero
public API changes):

1. `--until-loaded N` (already existed, P18): render up to N frames,
   stop early once `tilesLoading == 0 && tilesLoaded > 0` holds for 20
   consecutive frames; exit 1 if the budget is exhausted. Used by all
   static-camera pixel tests (golden MANIFEST, draco/ktx2 twin
   comparisons, and the per-format assertion tests via their `--frames`
   value reinterpreted as the give-up budget, 4x the old fixed count).
2. `--settle-before-screenshot N` (new, P30): for tests whose scenario
   needs a fixed frame count (frame-indexed assertions in
   `tileset_switch_test.py`), keep rendering up to N extra frames after
   the scenario until the same settle condition holds, then screenshot.
   The settle streak is tracked during the main loop too, so an
   already-converged scenario costs ~0 extra frames (measured +0 on the
   S3 reload path). In trajectory mode the camera keeps following the
   player (clamped past the last keyframe); the p22 switch trajectory is
   camera-static, so settled frames stay comparable with reference runs.

Deliberately NOT converted: failure-path tests (`fault_test.py`,
`tileset_failfast_test.py` — corrupt inputs never satisfy `loaded > 0`,
settle-waiting would invert their pass/fail), dynamics tests
(`stats`/`frustum_lod`/`memory_roam`/`weaknet`/`add_region`/
`trajectory` — fixed frames are the point), the P2 smoke test (no
tileset), and the sanitizer direct-demo invocations (memory gates, not
pixel gates).

## Verification

- Settle-gated `p7_i3dm` screenshot is **bit-identical** (0/480,000 px)
  to the frozen golden: convergence-gating changes *when* the shutter
  fires, not *what* it captures (static camera, deterministic pipeline,
  no frame-count-dependent render state — verified: no exposure
  adaptation in the codebase).
- `golden_regression` + `tileset_switch`: 3 consecutive clean runs.
- Full suite 27/27 green; total time ~160 s vs ~155 s before (< 50%
  increase budget; `--until-loaded` usually stops *earlier* than the old
  fixed count, e.g. p7 settles at frame 21 of 240).
- Golden baselines NOT re-frozen (all entries still IDENTICAL at
  tolerance 0).

## Consequences

- The fixed-frame screenshot race is closed as a flake source. If a
  screenshot test fails now, it is either a real rendering regression
  or a genuine convergence failure (the demo exits 1 with
  "NOT settled", naming the cause).
- Test authors: new pixel tests must use one of the two mechanisms,
  never a bare `--frames` + `--screenshot`.
