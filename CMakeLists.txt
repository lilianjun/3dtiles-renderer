cmake_minimum_required(VERSION 3.21)

project(tiles_renderer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Target platform selection.
# Values: linux | windows | android | ios | wasm
# ---------------------------------------------------------------------------
set(TILES_PLATFORM "linux" CACHE STRING "Target platform")
set_property(CACHE TILES_PLATFORM PROPERTY STRINGS linux windows android ios wasm)

# ---------------------------------------------------------------------------
# Dependency switches.
# P0: OFF by default so configure/build works with zero downloads.
# P1: pass -DTILES_WITH_SDL3=ON -DTILES_WITH_CESIUM_NATIVE=ON -DTILES_WITH_FILAMENT=ON
#     (or set them in your own CMakePresets overlay).
# Locked versions, see docs/adr/0002-toolchain.md.
#
# Dependency ownership (docs/adr/0003-sdk-no-sdl.md):
#   SDK static library (tiles_renderer): cesium-native + Filament. NO SDL3.
#   SDL demo (tiles_demo) + tests:      SDL3 only (as consumers).
# ---------------------------------------------------------------------------
option(TILES_WITH_CESIUM_NATIVE "Fetch and build cesium-native (3D Tiles loading/scheduling)" OFF)
option(TILES_WITH_FILAMENT      "Fetch Filament (rendering, prebuilt binaries)" OFF)
option(TILES_WITH_SDL3          "Fetch and build SDL3 (demo + tests only; never the SDK)" OFF)

# ---------------------------------------------------------------------------
# SDK availability checks.
# Policy: warn with a clear hint, never hard-fail. The host stub build
# always configures so developers get fast feedback on any machine.
# ---------------------------------------------------------------------------
if(TILES_PLATFORM STREQUAL "android")
  if(DEFINED ENV{ANDROID_NDK_HOME})
    set(_ndk "$ENV{ANDROID_NDK_HOME}")
  elseif(DEFINED ENV{ANDROID_NDK_ROOT})
    set(_ndk "$ENV{ANDROID_NDK_ROOT}")
  endif()
  if(NOT _ndk)
    message(WARNING
      "[P0] ANDROID_NDK_HOME/ANDROID_NDK_ROOT is not set — Android SDK check skipped.\n"
      "      Configure continues as a HOST STUB build. For a real cross build:\n"
      "        export ANDROID_NDK_HOME=<path-to-ndk>\n"
      "      then re-run: cmake --preset android")
  else()
    message(STATUS "[P0] Android NDK found: ${_ndk}")
  endif()
elseif(TILES_PLATFORM STREQUAL "ios")
  if(NOT APPLE)
    message(WARNING
      "[P0] iOS builds require macOS + Xcode — this host cannot produce an iOS binary.\n"
      "      Configure continues as a HOST STUB build.")
  endif()
elseif(TILES_PLATFORM STREQUAL "wasm")
  find_program(EMCC_EXECUTABLE emcc)
  if(NOT EMCC_EXECUTABLE)
    message(WARNING
      "[P0] 'emcc' not found — install & activate the Emscripten SDK for a real WASM build.\n"
      "      Configure continues as a HOST STUB build.")
  else()
    message(STATUS "[P0] Emscripten found: ${EMCC_EXECUTABLE}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Version header (generated)
# ---------------------------------------------------------------------------
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/tiles_renderer/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/tiles_renderer/version.h"
  @ONLY
)

# ---------------------------------------------------------------------------
# SDK static library.
# Public API: include/tiles_renderer/renderer.h — takes a platform-native
# window handle + size from the host app. The SDK NEVER depends on SDL3.
# ---------------------------------------------------------------------------
add_library(tiles_renderer STATIC src/renderer.cpp)
target_include_directories(tiles_renderer PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_features(tiles_renderer PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# Third-party dependencies (P1: locked to stable tags — docs/adr/0002).
# ---------------------------------------------------------------------------
include(FetchContent)

# --- cesium-native: 3D Tiles loading/scheduling/LOD. Goes into the SDK. ---
# Built from source at tag v0.64.0. Its own CMake (ezvcpkg) automatically
# provisions its third-party dependencies via vcpkg on first configure —
# expect the first configure to take a while (vcpkg builds ~30 ports).
if(TILES_WITH_CESIUM_NATIVE)
  message(STATUS "[P1] Fetching cesium-native @ v0.64.0 ...")
  # Skip cesium-native's own (large) test suite — we only need its libraries.
  set(CESIUM_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    cesium_native
    GIT_REPOSITORY https://github.com/CesiumGS/cesium-native.git
    GIT_TAG        v0.64.0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(cesium_native)
  # PUBLIC: consumers of this static SDK library need the transitive link.
  # Link the tile-selection core; its link interface pulls the rest transitively.
  target_link_libraries(tiles_renderer PUBLIC Cesium3DTilesSelection)
  target_compile_definitions(tiles_renderer PRIVATE TILES_WITH_CESIUM_NATIVE)
endif()

# --- Filament: rendering. Goes into the SDK. ---
# Filament is very expensive to build from source, so P1 consumes the
# official prebuilt binaries for the pinned release (per-platform archives).
# Linux today; android/ios/web/windows archives get wired in P2.
if(TILES_WITH_FILAMENT)
  if(TILES_PLATFORM STREQUAL "linux")
    set(TILES_FILAMENT_VERSION "1.77.0")
    message(STATUS "[P1] Fetching Filament prebuilt binaries v${TILES_FILAMENT_VERSION} (linux) ...")
    FetchContent_Declare(
      filament_bin
      URL https://github.com/google/filament/releases/download/v${TILES_FILAMENT_VERSION}/filament-v${TILES_FILAMENT_VERSION}-linux.tgz
    )
    FetchContent_MakeAvailable(filament_bin)
    add_library(filament_prebuilt INTERFACE)
    target_include_directories(filament_prebuilt INTERFACE
      "${filament_bin_SOURCE_DIR}/include")
    # Static libs; the linker pulls only referenced objects.
    target_link_directories(filament_prebuilt INTERFACE
      "${filament_bin_SOURCE_DIR}/lib/x86_64")
    target_link_libraries(filament_prebuilt INTERFACE
      filament backend bluegl bluevk filabridge filaflat
      utils geometry smol-v ibl)
    # PUBLIC: consumers of this static SDK library need the transitive link.
    target_link_libraries(tiles_renderer PUBLIC filament_prebuilt)
    target_compile_definitions(tiles_renderer PRIVATE TILES_WITH_FILAMENT)
  else()
    message(STATUS
      "[P1] Filament prebuilt binaries for platform '${TILES_PLATFORM}' "
      "are wired in P2 — skipping Filament on this preset.")
  endif()
endif()

# --- SDL3: windowing/input for the demo + tests ONLY. Never the SDK. ---
# (docs/adr/0003-sdk-no-sdl.md)
if(TILES_WITH_SDL3)
  message(STATUS "[P1] Fetching SDL3 @ release-3.4.16 (demo + tests only) ...")
  # Headless-friendly: SDL3's X11 check hard-fails when X11 dev packages are
  # only partially installed (typical on devservers/CI). Native video backends
  # are unnecessary for the P1 demo; desktop devs can re-enable with
  # -DTILES_SDL3_NATIVE_VIDEO=ON.
  option(TILES_SDL3_NATIVE_VIDEO "SDL3: enable native video backends (X11/Wayland)" OFF)
  if(NOT TILES_SDL3_NATIVE_VIDEO)
    # Console/headless build: no X11/Wayland dev packages required at all.
    set(SDL_X11 OFF CACHE BOOL "" FORCE)
    set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
  endif()
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.16
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(sdl3)
endif()

# ---------------------------------------------------------------------------
# SDL demo app: owns the window + input via SDL3, hands the SDK a native
# window handle. Reference integration for host apps; used for four-platform
# validation. Only built when SDL3 is enabled.
# ---------------------------------------------------------------------------
if(TILES_WITH_SDL3)
  add_executable(tiles_demo samples/demo/main.cpp)
  target_link_libraries(tiles_demo PRIVATE tiles_renderer SDL3::SDL3-static)
  target_compile_definitions(tiles_demo PRIVATE TILES_WITH_SDL3)
  target_compile_features(tiles_demo PRIVATE cxx_std_20)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

message(STATUS "[P0] Configured tiles_renderer ${PROJECT_VERSION} for platform '${TILES_PLATFORM}'")
