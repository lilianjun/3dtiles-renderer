cmake_minimum_required(VERSION 3.21)

project(tiles_renderer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Target platform selection.
# P0: informational only — real cross toolchains are wired in P1.
# Values: linux | windows | android | ios | wasm
# ---------------------------------------------------------------------------
set(TILES_PLATFORM "linux" CACHE STRING "Target platform")
set_property(CACHE TILES_PLATFORM PROPERTY STRINGS linux windows android ios wasm)

# ---------------------------------------------------------------------------
# Dependency switches.
# P0: OFF by default so configure/build works with zero downloads.
# P1: turn these ON to actually fetch and link the dependencies.
# ---------------------------------------------------------------------------
option(TILES_WITH_CESIUM_NATIVE "Fetch and build cesium-native (3D Tiles loading/scheduling)" OFF)
option(TILES_WITH_FILAMENT      "Fetch and build Filament (rendering)" OFF)
option(TILES_WITH_SDL3          "Fetch and build SDL3 (windowing/input)" OFF)

# ---------------------------------------------------------------------------
# SDK availability checks.
# P0 policy: warn with a clear hint, never hard-fail. The host stub build
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
# Main target
# ---------------------------------------------------------------------------
add_executable(tiles_renderer src/main.cpp)
target_include_directories(tiles_renderer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_features(tiles_renderer PRIVATE cxx_std_20)

# ---------------------------------------------------------------------------
# Third-party dependencies (placeholders — enabled in P1)
# ---------------------------------------------------------------------------
include(FetchContent)

if(TILES_WITH_CESIUM_NATIVE)
  message(STATUS "[P1] Fetching cesium-native ...")
  FetchContent_Declare(
    cesium_native
    GIT_REPOSITORY https://github.com/CesiumGS/cesium-native.git
    GIT_TAG        main
  )
  FetchContent_MakeAvailable(cesium_native)
endif()

if(TILES_WITH_FILAMENT)
  message(STATUS "[P1] Fetching filament ...")
  FetchContent_Declare(
    filament
    GIT_REPOSITORY https://github.com/google/filament.git
    GIT_TAG        main
  )
  FetchContent_MakeAvailable(filament)
endif()

if(TILES_WITH_SDL3)
  message(STATUS "[P1] Fetching SDL3 ...")
  FetchContent_Declare(
    sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        main
  )
  FetchContent_MakeAvailable(sdl3)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

message(STATUS "[P0] Configured tiles_renderer ${PROJECT_VERSION} for platform '${TILES_PLATFORM}'")
