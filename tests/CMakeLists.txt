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
