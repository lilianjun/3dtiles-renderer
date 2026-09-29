# P1 test suite.
#
# The smoke test links the SDK static library (proving it links). SDL3 is
# linked into tests only as a consumer — never into the SDK itself
# (docs/adr/0003-sdk-no-sdl.md).

add_executable(tiles_renderer_smoke smoke.cpp)
# SDK include dirs (public headers + generated version.h) come transitively
# from the tiles_renderer target.
target_link_libraries(tiles_renderer_smoke PRIVATE tiles_renderer)
target_compile_features(tiles_renderer_smoke PRIVATE cxx_std_20)

if(TILES_WITH_SDL3)
  target_link_libraries(tiles_renderer_smoke PRIVATE SDL3::SDL3-static)
  target_compile_definitions(tiles_renderer_smoke PRIVATE TILES_WITH_SDL3)
endif()

add_test(NAME smoke COMMAND tiles_renderer_smoke)
tiles_apply_sanitizers(tiles_renderer_smoke)

# The SDL demo must at least start and exit cleanly (headless CI uses SDL's
# dummy video driver; the demo handles the missing native handle gracefully).
if(TARGET tiles_demo)
  add_test(NAME demo_runs COMMAND tiles_demo --frames 5 --no-tileset)
endif()

# P2: headless render + screenshot assertion (Linux + SDL demo only).
# The python script wraps itself in xvfb-run when no X display is present,
# so this test works under both plain `ctest` and `xvfb-run ctest`.
# --no-tileset keeps the original P2 clear+triangle scene.
if(TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux")
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME demo_screenshot
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/screenshot_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --out "${CMAKE_BINARY_DIR}/screenshots/p2_demo.png"
              --frames 30 --width 800 --height 600
              --extra-args --no-tileset)
    set_tests_properties(demo_screenshot PROPERTIES TIMEOUT 900)
  else()
    message(WARNING "[P2] Python3 not found — demo_screenshot test skipped")
  endif()
endif()

# P3: headless tileset render + screenshot assertion (Linux + SDL demo only).
# Renders the vendored tiny tileset and asserts both LOD child tiles
# (orange + teal) are visible — provably different from the P2 image.
if(TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux" AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME tileset_screenshot
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tileset_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p3_box_tileset/tileset.json"
              --out "${CMAKE_BINARY_DIR}/screenshots/p3_tileset.png"
              --frames 60 --width 800 --height 600)
    set_tests_properties(tileset_screenshot PROPERTIES TIMEOUT 900)
  else()
    message(WARNING "[P3] Python3 not found — tileset_screenshot test skipped")
  endif()
endif()

# P5: fill the three P3 gaps, each with its own headless pixel assertion.
if(TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux" AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    # Gap 1 — HTTP(S) loading: serve the p3 tileset over a local HTTP server
    # (no external network) and reuse tileset_test.py's assertions.
    add_test(NAME http_tileset_screenshot
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/http_tileset_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --out "${CMAKE_BINARY_DIR}/screenshots/p5_http.png"
              --frames 60 --width 800 --height 600)
    set_tests_properties(http_tileset_screenshot PROPERTIES TIMEOUT 900)
    if(TILES_SANITIZE)
      # The linux-asan preset instruments the demo binary itself, so this
      # plain test also runs under LSan. Apply the same third-party
      # suppressions the sanitizer_* scenarios use (tests/lsan.supp):
      # without it, the documented cesium-native CurlAssetAccessor
      # handle-cache leak (ADR-0007, Entry 1) fails this test. Pre-existing
      # harness gap found while validating P8; the leak is third-party,
      # not ours. Only the HTTP test needs this — no other plain test
      # touches libcurl.
      set_tests_properties(http_tileset_screenshot PROPERTIES
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
    endif()

    # Gap 2 — b3dm: child_a is a real b3dm (BATCH_LENGTH=1); the converter
    # path (b3dm -> CesiumGltf::Model -> glb -> gltfio) must render orange.
    add_test(NAME b3dm_tileset_screenshot
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tileset_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p5_b3dm_tileset/tileset.json"
              --out "${CMAKE_BINARY_DIR}/screenshots/p5_b3dm.png"
              --frames 60 --width 800 --height 600)
    set_tests_properties(b3dm_tileset_screenshot PROPERTIES TIMEOUT 900)

    # Gap 3 — ECEF-style rebase: the `far` tileset's root tile is translated
    # by 123456789 m; without the rebase nothing would be visible. The test
    # renders `near` and `far` and requires the orange centroid to match.
    add_test(NAME rebase_tileset_screenshot
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/rebase_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --near-out "${CMAKE_BINARY_DIR}/screenshots/p5_rebase_near.png"
              --far-out "${CMAKE_BINARY_DIR}/screenshots/p5_rebase_far.png"
              --frames 60 --width 800 --height 600)
    set_tests_properties(rebase_tileset_screenshot PROPERTIES TIMEOUT 900)
  else()
    message(WARNING "[P5] Python3 not found — P5 screenshot tests skipped")
  endif()
endif()

# P7: i3dm real support — instancing, 256-instance stress, tile-transform
# rebase, and RTC_CENTER, all with headless pixel assertions.
if(TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux" AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME i3dm_tileset_screenshot
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/i3dm_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --out "${CMAKE_BINARY_DIR}/screenshots/p7_i3dm_main.png"
              --many-out "${CMAKE_BINARY_DIR}/screenshots/p7_i3dm_many.png"
              --rebase-near-out "${CMAKE_BINARY_DIR}/screenshots/p7_i3dm_rebase_near.png"
              --rebase-far-out "${CMAKE_BINARY_DIR}/screenshots/p7_i3dm_rebase_far.png"
              --rtc-out "${CMAKE_BINARY_DIR}/screenshots/p7_i3dm_rtc.png"
              --rtc-ref-out "${CMAKE_BINARY_DIR}/screenshots/p7_i3dm_rtc_ref.png"
              --frames 60 --width 800 --height 600)
    set_tests_properties(i3dm_tileset_screenshot PROPERTIES TIMEOUT 1200)
  else()
    message(WARNING "[P7] Python3 not found — i3dm screenshot test skipped")
  endif()
endif()

# P8: pnts (Point Cloud) real support — 420-point color cloud, 60000-point
# stress, tile-transform rebase, and RTC_CENTER, all with headless pixel
# assertions. No SDK code change was needed: Filament v1.77's gltfio +
# ubershader render POINTS primitives natively (1 px per point, exact
# sRGB colors), and the SDK's generic converted-model path (P5 RTC
# extraction + P7 multi-buffer merge) already covers pnts.
if(TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux" AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME pnts_tileset_screenshot
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/pnts_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --out "${CMAKE_BINARY_DIR}/screenshots/p8_pnts_main.png"
              --many-out "${CMAKE_BINARY_DIR}/screenshots/p8_pnts_many.png"
              --rebase-near-out "${CMAKE_BINARY_DIR}/screenshots/p8_pnts_rebase_near.png"
              --rebase-far-out "${CMAKE_BINARY_DIR}/screenshots/p8_pnts_rebase_far.png"
              --rtc-out "${CMAKE_BINARY_DIR}/screenshots/p8_pnts_rtc.png"
              --rtc-ref-out "${CMAKE_BINARY_DIR}/screenshots/p8_pnts_rtc_ref.png"
              --frames 60 --width 800 --height 600)
    set_tests_properties(pnts_tileset_screenshot PROPERTIES TIMEOUT 1200)
  else()
    message(WARNING "[P8] Python3 not found — pnts screenshot test skipped")
  endif()
endif()

# P9: cmpt (Composite) real support — one composite.cmpt packing a b3dm,
# a pnts and an i3dm inner tile. cesium-native's CmptToGltfConverter
# converts each inner tile recursively and merges the Models; the SDK's
# existing modelToGlb path handles the merged result with no code change.
if(TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux" AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME cmpt_tileset_screenshot
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/cmpt_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --out "${CMAKE_BINARY_DIR}/screenshots/p9_cmpt.png"
              --frames 60 --width 800 --height 600)
    set_tests_properties(cmpt_tileset_screenshot PROPERTIES TIMEOUT 1200)
  else()
    message(WARNING "[P9] Python3 not found — cmpt screenshot test skipped")
  endif()
endif()

# P10: 3D Tiles 1.1 — bare-glb content (3DTILES_content_gltf) and implicit
# QUADTREE (3DTILES_implicit_tiling) with a hand-written JSON subtree.
# cesium-native handles both with no SDK code change; the test asserts
# real pixels for both scenarios.
if(TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux" AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME tiles11_screenshot
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tiles11_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --outdir "${CMAKE_BINARY_DIR}/screenshots/p10_11"
              --frames 90 --width 800 --height 600)
    set_tests_properties(tiles11_screenshot PROPERTIES TIMEOUT 1200)
  else()
    message(WARNING "[P10] Python3 not found — 1.1 screenshot test skipped")
  endif()
endif()

# P15: PBR material pipeline verification — deterministic fixtures with
# metallic/roughness contrast, a hand-made normal map, BLEND transparency,
# single/double-sided culling and a procedural checkerboard baseColor
# texture, rendered through tiles_demo with comparison-based assertions
# (no hard-coded Mesa shading values). See docs/adr/0013-pbr-materials.md.
if(TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux" AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME pbr_materials_screenshot
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/pbr_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --outdir "${CMAKE_BINARY_DIR}/screenshots/p15_pbr"
              --frames 60 --width 800 --height 600)
    set_tests_properties(pbr_materials_screenshot PROPERTIES TIMEOUT 1200)
  else()
    message(WARNING "[P15] Python3 not found — pbr_materials_screenshot test skipped")
  endif()
endif()

# P16: deterministic camera trajectory replay (ADR-0014). The demo drives
# setOrbitCamera per logical frame from a CSV keyframe trajectory (smoothstep
# interpolation, no wall clock); the test runs the same 12-frame trajectory
# twice and requires every frame PNG bit-identical, with both child tiles
# in view on first/middle/last frames.
# Pixel-strict like golden_regression: not registered under TILES_SANITIZE.
if(NOT TILES_SANITIZE AND TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux"
   AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME trajectory_determinism
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/trajectory_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --workdir "${CMAKE_BINARY_DIR}/trajectory_tmp")
    set_tests_properties(trajectory_determinism PROPERTIES TIMEOUT 1200)
  else()
    message(WARNING "[P16] Python3 not found — trajectory_determinism test skipped")
  endif()
endif()

# P17: tile streaming diagnostics (ADR-0015). Drives tiles_demo --stats and
# asserts: healthy tileset -> loaded > 0, failed == 0, loaded monotonic,
# renderedTileCount() consistent with selected; corrupt glb -> failed > 0
# with graceful exit 0; and querying stats never perturbs pixels
# (screenshots with/without --stats are bit-identical).
# Pixel-strict like golden_regression: not registered under TILES_SANITIZE
# (see the sanitizer_stats scenario below for the ASan lane).
if(NOT TILES_SANITIZE AND TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux"
   AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME tile_stats
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/stats_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p3_box_tileset/tileset.json"
              --outdir "${CMAKE_BINARY_DIR}/screenshots/p17_stats"
              --frames 90 --width 800 --height 600)
    set_tests_properties(tile_stats PROPERTIES TIMEOUT 1200)
  else()
    message(WARNING "[P17] Python3 not found — tile_stats test skipped")
  endif()
endif()

# P18: weak-network resilience gate (local slow HTTP fixture, no external
# network/root). Covers: slow-net completion, request prevention on
# deselection, mid-load camera change, mid-load teardown, and server outage.
# Deterministic steady-state assertions; the whole suite runs < 2 min.
if(NOT TILES_SANITIZE AND TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux"
   AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME weak_network
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/weaknet_test.py"
              --mode all
              --demo "$<TARGET_FILE:tiles_demo>")
    # Five cases incl. a 3s dead-server window; comfortably under 2 minutes.
    set_tests_properties(weak_network PROPERTIES TIMEOUT 600)
  else()
    message(WARNING "[P18] Python3 not found — weak_network test skipped")
  endif()
endif()

# P19: camera-roam memory boundedness. Multi-lap yaw roam over the deep
# implicit fixture (341 tiles, ~15.6MB) with an 8MB cache budget; proves
# LRU eviction keeps tilesLoaded/bytesLoaded bounded while the request log
# proves wide coverage (253 distinct tiles, 629 hits = eviction churn).
# Not run under TILES_SANITIZE: the ASan-instrumented binary's RSS is not
# representative (see sanitizer_memory_roam below for the sanitizer gate).
if(NOT TILES_SANITIZE AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME memory_roam
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/memory_roam_test.py"
              --demo "$<TARGET_FILE:tiles_demo>")
    # 3 laps + warmup; comfortably under 2 minutes.
    set_tests_properties(memory_roam PROPERTIES TIMEOUT 600)
  else()
    message(WARNING "[P19] Python3 not found — memory_roam test skipped")
  endif()
endif()

# P20: frustum culling + LOD refinement. Two-part test (ground-plane fixture
# for per-tile frustum set flips; tiny REPLACE fixture for parent->child
# selection switching). Fast (<5s); run 3x for stability during development.
if(NOT TILES_SANITIZE AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME frustum_lod
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/frustum_lod_test.py"
              --demo "$<TARGET_FILE:tiles_demo>")
    set_tests_properties(frustum_lod PROPERTIES TIMEOUT 300)
  else()
    message(WARNING "[P20] Python3 not found — frustum_lod test skipped")
  endif()
endif()

# P21: ADD refinement + region bounding volumes. Two-part test (ADD
# fixture for parent-stays-selected semantics vs P20's REPLACE;
# region fixture: WGS84 region BVs + tile transform at ECEF magnitude,
# rebase brings them back near the origin). Fast (<10s incl. the
# empty-reference screenshot render); run 3x for stability during dev.
if(NOT TILES_SANITIZE AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME add_region
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/add_region_test.py"
              --demo "$<TARGET_FILE:tiles_demo>")
    set_tests_properties(add_region PROPERTIES TIMEOUT 300)
  else()
    message(WARNING "[P21] Python3 not found — add_region test skipped")
  endif()
endif()

# P22: loadTileset re-entry / switching safety. Four scenarios
# (clean A->B switch with pixel proof, failed-switch-then-retry, reload
# same path, switch away mid slow-HTTP load). Fast (<60s for 3 reps);
# run 3x for stability during development.
if(NOT TILES_SANITIZE AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME tileset_switch
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tileset_switch_test.py"
              --demo "$<TARGET_FILE:tiles_demo>")
    set_tests_properties(tileset_switch PROPERTIES TIMEOUT 600)
  else()
    message(WARNING "[P22] Python3 not found — tileset_switch test skipped")
  endif()
endif()

# P23: corrupt-tileset.json fail-fast gate. Corrupt-but-present root
# documents must fail loadTileset() in milliseconds (pre-flight
# validation), never spin the 30s root-wait; the live tileset keeps
# rendering and the end screenshot is bit-identical to a no-switch run.
# Scenarios: garbage bytes, valid-JSON-not-a-tileset, truncated JSON,
# corrupt over slow HTTP (0.3s/response — also proves the P18 slow path
# is not mis-killed by the pre-flight).
if(NOT TILES_SANITIZE AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME tileset_failfast
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tileset_failfast_test.py"
              --demo "$<TARGET_FILE:tiles_demo>")
    set_tests_properties(tileset_failfast PROPERTIES TIMEOUT 900)
  else()
    message(WARNING "[P23] Python3 not found — tileset_failfast test skipped")
  endif()
endif()

# P25: KTX2 (KHR_texture_basisu) texture support gate. Deterministic
# UASTC fixture (tests/data/gen_p25_ktx2.py): the KTX2-textured box must
# render and match its PNG twin within a tight tolerance (the embedded
# UASTC decodes losslessly for this flat-color checkerboard), and a
# corrupt KTX2 must fail gracefully (exit 0, no crash/hang).
if(NOT TILES_SANITIZE AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME ktx2
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/ktx2_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --out "${CMAKE_BINARY_DIR}/screenshots/p25_ktx2")
    set_tests_properties(ktx2 PROPERTIES TIMEOUT 900)
  else()
    message(WARNING "[P25] Python3 not found — ktx2 test skipped")
  endif()
endif()

# P11: golden screenshot regression gate. Re-renders one representative
# fixture per phase with fixed parameters and requires pixel equality
# against the frozen goldens in tests/golden/ (see its README for the
# comparison policy and the regeneration rules).
# Deliberately NOT registered under TILES_SANITIZE: ASan/UBSan
# instrumentation can perturb pixel output; sanitizer builds gate on
# memory correctness, the golden gate on pixel correctness — separate
# lanes, each strict in its own.
if(NOT TILES_SANITIZE AND TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux"
   AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME golden_regression
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/golden_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --workdir "${CMAKE_BINARY_DIR}/golden_tmp")
    set_tests_properties(golden_regression PROPERTIES TIMEOUT 1200)
  else()
    message(WARNING "[P11] Python3 not found — golden_regression test skipped")
  endif()
endif()

# P6: lifecycle + fault-injection tests. Meaningful with or without
# sanitizers (they assert graceful failure instead of crashes); the
# sanitizer_* variants below additionally gate on ASan/LSan/UBSan reports.
if(TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux")
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    # C++ API lifecycle test: needs a real X11 window, so it re-execs
    # itself under xvfb-run when headless.
    if(TILES_WITH_SDL3)
      add_executable(tiles_renderer_lifecycle lifecycle.cpp)
      target_link_libraries(tiles_renderer_lifecycle PRIVATE
        tiles_renderer SDL3::SDL3-static)
      target_compile_definitions(tiles_renderer_lifecycle PRIVATE TILES_WITH_SDL3)
      target_compile_features(tiles_renderer_lifecycle PRIVATE cxx_std_20)
      tiles_apply_sanitizers(tiles_renderer_lifecycle)
      add_test(NAME lifecycle COMMAND tiles_renderer_lifecycle)
      set_tests_properties(lifecycle PROPERTIES TIMEOUT 600)

      # P12: host-integration check — drives the public API like a real
      # host app (init -> render -> resize -> load -> shutdown), covering
      # the P12 additions Renderer::resize() and Renderer::lastError().
      # Needs a real X11 window (self-wraps under xvfb-run when headless).
      # Uses only the public header; argv[1] is a loadable fixture dir.
      add_executable(tiles_renderer_host_integration host_integration_check.cpp)
      target_link_libraries(tiles_renderer_host_integration PRIVATE
        tiles_renderer SDL3::SDL3-static)
      target_compile_features(tiles_renderer_host_integration PRIVATE cxx_std_20)
      tiles_apply_sanitizers(tiles_renderer_host_integration)
      add_test(NAME host_integration
        COMMAND tiles_renderer_host_integration
                "${CMAKE_CURRENT_SOURCE_DIR}/data/p8_pnts_cloud")
      set_tests_properties(host_integration PROPERTIES TIMEOUT 600)
    endif()
    # Fault injection through the demo binary: bad tileset path, corrupt
    # tileset.json, corrupt .glb — all must fail gracefully, never crash.
    add_test(NAME fault_inputs
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/fault_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --workdir "${CMAKE_BINARY_DIR}/fault_tmp")
    set_tests_properties(fault_inputs PROPERTIES TIMEOUT 900)
  else()
    message(WARNING "[P6] Python3 not found — lifecycle/fault tests skipped")
  endif()
endif()

# P6: sanitizer scenario gate. Only registered when TILES_SANITIZE=ON
# (linux-asan preset): each scenario runs under ASan+LSan+UBSan and fails
# on any sanitizer report or unexpected exit code.
if(TILES_SANITIZE AND TARGET tiles_demo AND TILES_PLATFORM STREQUAL "linux"
   AND TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    set(_p6_supp "${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
    set(_p6_run "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/run_sanitized.py"
        --suppressions "${_p6_supp}")
    # P2 scene: clear color + unlit triangle, plain render + shutdown.
    add_test(NAME sanitizer_demo
      COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_demo>"
              --frames 30 --no-tileset)
    # P3 scene: local tileset load + tile selection/LOD + gltfio assets.
    add_test(NAME sanitizer_tileset
      COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_demo>"
              --frames 60
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p3_box_tileset/tileset.json")
    # P5 scenes: b3dm conversion path, and HTTP loading (local server).
    add_test(NAME sanitizer_b3dm
      COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_demo>"
              --frames 60
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p5_b3dm_tileset/tileset.json")
    # P7 scene: i3dm conversion path (instancing expansion + up-axis fix).
    add_test(NAME sanitizer_i3dm
      COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_demo>"
              --frames 60
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p7_i3dm_tileset/tileset.json")
    # P8 scene: pnts conversion path (POINTS primitive, native Filament
    # points rendering; 60000-point cloud for the sanitizer build).
    add_test(NAME sanitizer_pnts
      COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_demo>"
              --frames 60
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p8_pnts_many/tileset.json")
    # P9 scene: cmpt conversion path (composite of b3dm + pnts + i3dm,
    # merged by cesium-native into one Model; no SDK code change).
    add_test(NAME sanitizer_cmpt
      COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_demo>"
              --frames 60
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p9_cmpt_tileset/tileset.json")
    # P10 scene: 3D Tiles 1.1 implicit quadtree (subtree JSON +
    # per-tile glb content through BinaryToGltfConverter; no SDK change).
    add_test(NAME sanitizer_11
      COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_demo>"
              --frames 60
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p10_11_implicit/tileset.json")
    # P15 scene: PBR materials with embedded PNG textures (checkerboard
    # baseColor + normal map) through the stb TextureProvider added in P15.
    add_test(NAME sanitizer_pbr
      COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_demo>"
              --frames 60
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p15_tex/tileset.json")
    # P16 scene: trajectory replay (CSV keyframes -> per-frame orbit camera)
    # with per-frame PNG dumps, exercising the new demo code paths.
    add_test(NAME sanitizer_trajectory
      COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_demo>"
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p3_box_tileset/tileset.json"
              --trajectory "${CMAKE_CURRENT_SOURCE_DIR}/data/trajectories/p16_orbit_push.csv"
              --frame-dir "${CMAKE_BINARY_DIR}/sanitizer_traj_tmp")
    # P17 scene: tile streaming diagnostics — the new TileStats tree walk
    # (Done/Failed counting) plus the demo's --stats print path, all under
    # ASan/LSan/UBSan. Pixel comparison stays in the plain tile_stats test.
    add_test(NAME sanitizer_stats
      COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_demo>"
              --frames 60 --stats
              --tileset "${CMAKE_CURRENT_SOURCE_DIR}/data/p3_box_tileset/tileset.json")
    # P18: mid-load teardown under ASan/LSan/UBSan — exit while tile content
    # requests are in flight, then gate on any sanitizer report. Uses the
    # same lsan.supp as the other sanitizer_* scenarios (third-party
    # CurlAssetAccessor handle; see tests/lsan.supp).
    add_test(NAME sanitizer_weaknet_abort
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/weaknet_test.py"
              --mode abort --sanitized
              --suppressions "${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
              --demo "$<TARGET_FILE:tiles_demo>")
    set_tests_properties(sanitizer_weaknet_abort PROPERTIES TIMEOUT 600)
    # P19: roam memory boundedness under ASan/LSan/UBSan — 150-frame roam
    # with cache budget, gating on sanitizer reports plus the boundedness
    # assertions (relaxed coverage for the shorter run).
    add_test(NAME sanitizer_memory_roam
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/memory_roam_test.py"
              --sanitized
              --suppressions "${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
              --demo "$<TARGET_FILE:tiles_demo>")
    set_tests_properties(sanitizer_memory_roam PROPERTIES TIMEOUT 900)
    # P20: frustum culling + LOD refinement under ASan/LSan/UBSan — shorter
    # runs gating on sanitizer reports plus the core assertions (count-based
    # culling for frustum, REPLACE child-selection for LOD).
    add_test(NAME sanitizer_frustum_lod
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/frustum_lod_test.py"
              --sanitized
              --suppressions "${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
              --demo "$<TARGET_FILE:tiles_demo>")
    set_tests_properties(sanitizer_frustum_lod PROPERTIES TIMEOUT 600)
    # P21: ADD refinement + region bounding volumes under ASan/LSan/UBSan —
    # shorter runs gating on sanitizer reports plus the core assertions
    # (ADD: root stays selected at NEAR; region: all tiles load, failed=0).
    add_test(NAME sanitizer_add_region
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/add_region_test.py"
              --sanitized
              --suppressions "${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
              --demo "$<TARGET_FILE:tiles_demo>")
    set_tests_properties(sanitizer_add_region PROPERTIES TIMEOUT 600)
    # P22: tileset switching under ASan/LSan/UBSan — teardown paths
    # (clean switch, failed-switch-then-retry, reload, mid-load abandon)
    # with the core assertions.
    add_test(NAME sanitizer_tileset_switch
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tileset_switch_test.py"
              --sanitized
              --suppressions "${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
              --demo "$<TARGET_FILE:tiles_demo>")
    set_tests_properties(sanitizer_tileset_switch PROPERTIES TIMEOUT 900)
    # P23: corrupt-tileset.json fail-fast under ASan/LSan/UBSan — the
    # pre-flight fetch/parse path plus the failed-switch teardown, with
    # the core fail-fast + scene-undisturbed assertions.
    add_test(NAME sanitizer_tileset_failfast
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tileset_failfast_test.py"
              --sanitized
              --suppressions "${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
              --demo "$<TARGET_FILE:tiles_demo>")
    set_tests_properties(sanitizer_tileset_failfast PROPERTIES TIMEOUT 900)
    # P25: KTX2 texture support under ASan/LSan/UBSan — the libktx-backed
    # provider (transcode + upload + shutdown) plus the corrupt-input path,
    # with the core KTX2-vs-PNG assertions.
    add_test(NAME sanitizer_ktx2
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/ktx2_test.py"
              --sanitized
              --demo "$<TARGET_FILE:tiles_demo>"
              --out "${CMAKE_BINARY_DIR}/screenshots/p25_ktx2_asan")
    set_tests_properties(sanitizer_ktx2 PROPERTIES TIMEOUT 900)
    add_test(NAME sanitizer_http
      COMMAND ${_p6_run} -- "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/http_tileset_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --out "${CMAKE_BINARY_DIR}/screenshots/p6_http.png"
              --frames 60)
    # P6: fault injection + API lifecycle, under sanitizers.
    add_test(NAME sanitizer_faults
      COMMAND ${_p6_run} -- "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/fault_test.py"
              --demo "$<TARGET_FILE:tiles_demo>"
              --workdir "${CMAKE_BINARY_DIR}/fault_tmp_asan")
    if(TARGET tiles_renderer_lifecycle)
      add_test(NAME sanitizer_lifecycle
        COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_renderer_lifecycle>")
      set_tests_properties(sanitizer_lifecycle PROPERTIES TIMEOUT 900)
    endif()
    # P12: host-integration check under sanitizers.
    if(TARGET tiles_renderer_host_integration)
      add_test(NAME sanitizer_host_integration
        COMMAND ${_p6_run} -- "$<TARGET_FILE:tiles_renderer_host_integration>"
                "${CMAKE_CURRENT_SOURCE_DIR}/data/p8_pnts_cloud")
      set_tests_properties(sanitizer_host_integration PROPERTIES TIMEOUT 900)
    endif()
    set_tests_properties(
      sanitizer_demo sanitizer_tileset sanitizer_b3dm sanitizer_i3dm
      sanitizer_pnts sanitizer_cmpt sanitizer_11 sanitizer_pbr sanitizer_http
      sanitizer_trajectory sanitizer_faults
      PROPERTIES TIMEOUT 900)
  else()
    message(WARNING "[P6] Python3 not found — sanitizer scenario tests skipped")
  endif()
endif()
