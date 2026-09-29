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
    set_tests_properties(
      sanitizer_demo sanitizer_tileset sanitizer_b3dm sanitizer_http
      sanitizer_faults
      PROPERTIES TIMEOUT 900)
  else()
    message(WARNING "[P6] Python3 not found — sanitizer scenario tests skipped")
  endif()
endif()
