cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# Target platform selection.
# Values: linux | windows | android | ios | wasm
# Declared before project() so the cross-toolchain wiring below can read it.
# ---------------------------------------------------------------------------
set(TILES_PLATFORM "linux" CACHE STRING "Target platform")
set_property(CACHE TILES_PLATFORM PROPERTY STRINGS linux windows android ios wasm)

# ---------------------------------------------------------------------------
# P4: cross-compilation toolchain auto-wiring (must run before project()).
# When the toolchain is found in the environment, the preset "just works".
# When it is missing, configure falls back to a HOST STUB build with a
# warning (P0 policy) instead of hard-failing. An explicitly passed
# -DCMAKE_TOOLCHAIN_FILE always wins over the auto-wiring.
#
# TILES_CROSS_TOOLCHAIN is recomputed deterministically on *every*
# configure from cached state + environment (never "remembered" via the
# cache), so re-configures (e.g. after editing CMakeLists.txt) behave
# identically to fresh ones.
# ---------------------------------------------------------------------------
# Step 1: detect an already-wired toolchain (re-configure or explicit -D).
set(TILES_CROSS_TOOLCHAIN "")
if(TILES_PLATFORM STREQUAL "android"
    AND CMAKE_TOOLCHAIN_FILE MATCHES "android\\.toolchain\\.cmake")
  set(TILES_CROSS_TOOLCHAIN "android-ndk")
elseif(TILES_PLATFORM STREQUAL "wasm"
    AND CMAKE_TOOLCHAIN_FILE MATCHES "Emscripten\\.cmake")
  set(TILES_CROSS_TOOLCHAIN "emscripten")
elseif(TILES_PLATFORM STREQUAL "ios" AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(TILES_CROSS_TOOLCHAIN "ios")
endif()
# Step 2: fresh auto-wiring when nothing is wired yet.
if(NOT TILES_CROSS_TOOLCHAIN AND NOT CMAKE_TOOLCHAIN_FILE)
  if(TILES_PLATFORM STREQUAL "android")
    if(DEFINED ENV{ANDROID_NDK_HOME}
        AND EXISTS "$ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake")
      set(CMAKE_TOOLCHAIN_FILE "$ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake")
      set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI")
      set(ANDROID_PLATFORM "android-24" CACHE STRING "Android API level")
      set(ANDROID_STL "c++_shared" CACHE STRING "Android STL")
      set(TILES_CROSS_TOOLCHAIN "android-ndk")
    endif()
  elseif(TILES_PLATFORM STREQUAL "wasm")
    if(DEFINED ENV{EMSDK}
        AND EXISTS "$ENV{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
      set(CMAKE_TOOLCHAIN_FILE "$ENV{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
      set(TILES_CROSS_TOOLCHAIN "emscripten")
    endif()
  elseif(TILES_PLATFORM STREQUAL "ios" AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    # Real iOS cross build on macOS hosts (CI). Xcode clang handles
    # -arch/-isysroot from these cache variables with Makefiles/Ninja too.
    set(CMAKE_SYSTEM_NAME "iOS" CACHE STRING "Target system")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "iOS architectures")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "iOS deployment target")
    set(TILES_CROSS_TOOLCHAIN "ios")
  endif()
endif()

project(tiles_renderer VERSION 0.1.0 LANGUAGES CXX)

# P4: Filament's windows prebuilt is built against the /MD runtime — our
# targets must match it, otherwise LNK2038 (RuntimeLibrary mismatch).
if(TILES_PLATFORM STREQUAL "windows" AND MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

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

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# SDK availability checks.
# Policy: warn with a clear hint, never hard-fail. The host stub build
# always configures so developers get fast feedback on any machine.
# (The actual toolchain wiring happens before project(); see above.)
# ---------------------------------------------------------------------------
if(TILES_PLATFORM STREQUAL "android")
  if(TILES_CROSS_TOOLCHAIN STREQUAL "android-ndk")
    message(STATUS "[P4] Android NDK toolchain wired: $ENV{ANDROID_NDK_HOME} (ABI=${ANDROID_ABI})")
  else()
    message(WARNING
      "[P0] ANDROID_NDK_HOME is not set or has no android.toolchain.cmake — Android SDK check skipped.\n"
      "      Configure continues as a HOST STUB build. For a real cross build:\n"
      "        export ANDROID_NDK_HOME=<path-to-ndk>\n"
      "      then re-run: cmake --preset android")
  endif()
elseif(TILES_PLATFORM STREQUAL "ios")
  if(TILES_CROSS_TOOLCHAIN STREQUAL "ios")
    message(STATUS "[P4] iOS cross toolchain wired (macOS host, arm64)")
  else()
    message(WARNING
      "[P0] iOS builds require macOS + Xcode — this host cannot produce an iOS binary.\n"
      "      Configure continues as a HOST STUB build.")
  endif()
elseif(TILES_PLATFORM STREQUAL "wasm")
  if(TILES_CROSS_TOOLCHAIN STREQUAL "emscripten")
    message(STATUS "[P4] Emscripten toolchain wired: $ENV{EMSDK}")
  else()
    message(WARNING
      "[P0] EMSDK not set or Emscripten.cmake not found — install & activate the Emscripten SDK for a real WASM build.\n"
      "      Configure continues as a HOST STUB build.")
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
add_library(tiles_renderer STATIC src/renderer.cpp src/tileset.cpp)
target_include_directories(tiles_renderer PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_features(tiles_renderer PUBLIC cxx_std_20)
# P4: the SDK source selects its Filament backend + native-handle
# interpretation from the *target* platform (see src/renderer.cpp).
string(TOUPPER "${TILES_PLATFORM}" _tiles_platform_upper)
target_compile_definitions(tiles_renderer PRIVATE
  "TILES_PLATFORM_${_tiles_platform_upper}")

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
# Filament is very expensive to build from source, so we consume the
# official prebuilt binaries for the pinned release (per-platform archives).
# P4 wires linux / android / windows / ios. wasm has no C++ release (the
# official web package is filament.js only), so Filament stays off there
# and the SDK builds as a stub — see docs/adr/0006-platform-backends.md.
if(TILES_WITH_FILAMENT)
  set(TILES_FILAMENT_VERSION "1.77.0")
  set(_filament_base_url
      "https://github.com/google/filament/releases/download/v${TILES_FILAMENT_VERSION}")

  # Per-platform selection. <pkg> = release tgz name, <libdir> = lib dir
  # inside it, <libs> = archive names (without lib prefix / extension).
  set(_filament_pkg "")
  set(_filament_libdir "")
  set(_filament_libs "")
  set(_filament_use_group OFF)   # GNU ld --start-group (not for MSVC/Xcode)
  set(_filament_xcframework OFF) # iOS: pick the device slice out of each .xcframework
  set(_filament_syslibs "")

  if(TILES_PLATFORM STREQUAL "linux")
    set(_filament_pkg "filament-v${TILES_FILAMENT_VERSION}-linux.tgz")
    set(_filament_libdir "lib/x86_64")
    set(_filament_libs filament backend bluegl bluevk filabridge filaflat
        utils geometry smol-v ibl gltfio gltfio_core uberarchive uberzlib image)
    set(_filament_use_group ON)
    # Note: the official Filament release binaries are built with
    # clang/libc++, so consumers also link the libc++ runtime (+ zstd used
    # by MaterialParser). Our own objects stay on libstdc++; no std:: types
    # cross the Filament API boundary (Engine::Config etc. are POD), so the
    # two runtimes coexist.
    set(_filament_syslibs zstd c++ c++abi)
  elseif(TILES_PLATFORM STREQUAL "android")
    if(NOT TILES_CROSS_TOOLCHAIN STREQUAL "android-ndk")
      message(WARNING "[P4] Filament's android prebuilt needs the NDK cross toolchain — skipping Filament (stub SDK).")
    else()
      set(_filament_pkg "filament-v${TILES_FILAMENT_VERSION}-android-native.tgz")
      set(_filament_libdir "lib/${ANDROID_ABI}")
      # The android prebuilt ships Vulkan only (no bluegl) and a single
      # gltfio_core (no separate gltfio archive).
      set(_filament_libs filament backend bluevk filabridge filaflat
          utils geometry smol-v ibl gltfio_core uberarchive uberzlib image)
      set(_filament_use_group ON)
      # NDK supplies libc++ via ANDROID_STL; zstd + android/log are extra.
      # GLESv3/EGL: Engine::create()'s backend switch pulls every driver
      # (incl. OpenGLDriver) into the link, so the system GL libs are a
      # genuine transitive dependency of the SDK on Android.
      set(_filament_syslibs zstd android log GLESv3 EGL)
    endif()
  elseif(TILES_PLATFORM STREQUAL "windows")
    set(_filament_pkg "filament-v${TILES_FILAMENT_VERSION}-windows.tgz")
    # Note the extra md/ level (libs built against the /MD runtime).
    set(_filament_libdir "lib/x86_64/md")
    set(_filament_libs filament backend bluevk filabridge filaflat
        utils geometry smol-v ibl gltfio gltfio_core uberarchive uberzlib image)
    # MSVC resolves cyclic static deps without --start-group.
    # zstd.lib ships in the package (used by MaterialParser, as on Linux).
    # ws2_32/dbghelp: usual Windows system deps of Filament's utils/backend.
    set(_filament_syslibs zstd ws2_32 dbghelp)
  elseif(TILES_PLATFORM STREQUAL "ios")
    if(NOT TILES_CROSS_TOOLCHAIN STREQUAL "ios")
      message(WARNING "[P4] Filament's ios prebuilt needs a macOS host — skipping Filament (stub SDK).")
    else()
      set(_filament_pkg "filament-v${TILES_FILAMENT_VERSION}-ios.tgz")
      set(_filament_xcframework ON)
      # NOTE: zstd lives in the xcframework too (no link_directories in this
      # layout), so it goes through _filament_libs as a full path as well.
      set(_filament_libs filament backend filabridge filaflat
          utils geometry smol-v ibl gltfio_core uberarchive uberzlib image zstd)
      # iOS system frameworks pulled in by Filament's drivers/platform code.
      set(_filament_syslibs
          "-framework Foundation" "-framework UIKit" "-framework Metal"
          "-framework CoreGraphics" "-framework CoreVideo"
          "-framework QuartzCore" "-framework IOKit" "-framework CFNetwork")
    endif()
  elseif(TILES_PLATFORM STREQUAL "wasm")
    message(STATUS "[P4] No C++ Filament release for Web (official web package is filament.js only) — Filament disabled on wasm (stub SDK). See docs/adr/0006-platform-backends.md.")
  endif()

  if(_filament_pkg)
    message(STATUS "[P4] Fetching Filament prebuilt binaries v${TILES_FILAMENT_VERSION} (${_filament_pkg}) ...")
    FetchContent_Declare(
      filament_bin
      URL "${_filament_base_url}/${_filament_pkg}"
    )
    FetchContent_MakeAvailable(filament_bin)
    add_library(filament_prebuilt INTERFACE)
    target_include_directories(filament_prebuilt INTERFACE
      "${filament_bin_SOURCE_DIR}/include")
    if(_filament_xcframework)
      # iOS ships .xcframeworks; link the ios-arm64 device slice directly
      # (works with Makefiles/Ninja — no Xcode project required).
      foreach(_lib IN LISTS _filament_libs)
        target_link_libraries(filament_prebuilt INTERFACE
          "${filament_bin_SOURCE_DIR}/lib/lib${_lib}.xcframework/ios-arm64/lib${_lib}.a")
      endforeach()
    else()
      # Static libs; the linker pulls only referenced objects.
      target_link_directories(filament_prebuilt INTERFACE
        "${filament_bin_SOURCE_DIR}/${_filament_libdir}")
      if(_filament_use_group)
        # The static archives reference each other cyclically (e.g. gltfio
        # -> filament/geometry); group them so GNU ld rescans until resolved.
        target_link_libraries(filament_prebuilt INTERFACE "-Wl,--start-group")
      endif()
      target_link_libraries(filament_prebuilt INTERFACE ${_filament_libs})
      if(_filament_use_group)
        target_link_libraries(filament_prebuilt INTERFACE "-Wl,--end-group")
      endif()
    endif()
    target_link_libraries(filament_prebuilt INTERFACE ${_filament_syslibs})
    # PUBLIC: consumers of this static SDK library need the transitive link.
    target_link_libraries(tiles_renderer PUBLIC filament_prebuilt)
    target_compile_definitions(tiles_renderer PRIVATE TILES_WITH_FILAMENT)

    # Host matc: compiles the embedded demo material. matc is a *host* tool,
    # so it comes from the host-platform tgz (reused when target == host).
    # -a all: one material blob serves every backend (OpenGL/Vulkan/Metal).
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
      set(_host_pkg "filament-v${TILES_FILAMENT_VERSION}-mac.tgz")
      set(_matc_rel "bin/matc")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
      set(_host_pkg "filament-v${TILES_FILAMENT_VERSION}-windows.tgz")
      set(_matc_rel "bin/matc.exe")
    else()
      set(_host_pkg "filament-v${TILES_FILAMENT_VERSION}-linux.tgz")
      set(_matc_rel "bin/matc")
    endif()
    if(_host_pkg STREQUAL _filament_pkg)
      set(TILES_MATC "${filament_bin_SOURCE_DIR}/${_matc_rel}")
    else()
      FetchContent_Declare(
        filament_host
        URL "${_filament_base_url}/${_host_pkg}"
      )
      FetchContent_MakeAvailable(filament_host)
      set(TILES_MATC "${filament_host_SOURCE_DIR}/${_matc_rel}")
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(TILES_MAT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/materials/unlit_color.mat")
    set(TILES_FILAMAT "${CMAKE_CURRENT_BINARY_DIR}/generated/unlit_color.filamat")
    set(TILES_FILAMAT_H
        "${CMAKE_CURRENT_BINARY_DIR}/generated/unlit_color_filamat.h")
    add_custom_command(
      OUTPUT "${TILES_FILAMAT}"
      COMMAND "${TILES_MATC}" -a all -o "${TILES_FILAMAT}" "${TILES_MAT_SRC}"
      DEPENDS "${TILES_MAT_SRC}"
      COMMENT "matc: compiling unlit_color.mat (all backends)")
    add_custom_command(
      OUTPUT "${TILES_FILAMAT_H}"
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_filamat.py"
              "${TILES_FILAMAT}" "${TILES_FILAMAT_H}" unlit_color_filamat
      DEPENDS "${TILES_FILAMAT}"
              "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_filamat.py"
      COMMENT "Embedding unlit_color.filamat as C++ header")
    add_custom_target(tiles_materials DEPENDS "${TILES_FILAMAT_H}")
    add_dependencies(tiles_renderer tiles_materials)

    # P4: cross platforms build no demo/tests, so nothing would ever link
    # the SDK — add a shared-lib link check to validate the prebuilt lib
    # list (paths + symbol resolution) on each wired platform.
    if(TILES_PLATFORM STREQUAL "android" OR TILES_PLATFORM STREQUAL "ios")
      add_library(tiles_renderer_linkcheck SHARED cmake/linkcheck.cpp)
      target_link_libraries(tiles_renderer_linkcheck PRIVATE tiles_renderer)
    endif()
  else()
    message(STATUS
      "[P4] Filament prebuilt binaries for platform '${TILES_PLATFORM}' "
      "are not wired — building the SDK without Filament (stub).")
  endif()
endif()

# --- SDL3: windowing/input for the demo + tests ONLY. Never the SDK. ---
# (docs/adr/0003-sdk-no-sdl.md)
if(TILES_WITH_SDL3)
  message(STATUS "[P1] Fetching SDL3 @ release-3.4.16 (demo + tests only) ...")
  # Headless-friendly default: SDL3's X11 check hard-fails when X11 dev
  # packages are only partially installed (typical on devservers/CI).
  # Native video backends are unnecessary for a pure link check; pass
  # -DTILES_SDL3_NATIVE_VIDEO=ON (with X11 dev packages installed) for real
  # windowing, e.g. rendering validation under xvfb.
  option(TILES_SDL3_NATIVE_VIDEO "SDL3: enable native video backends (X11)" OFF)
  if(TILES_SDL3_NATIVE_VIDEO)
    # Real X11 windowing. Requires: libx11-dev libxext-dev libxrandr-dev
    # libxcursor-dev libxi-dev (Wayland stays off: fewer host dependencies).
    set(SDL_X11 ON CACHE BOOL "" FORCE)
    set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
    set(SDL_UNIX_CONSOLE_BUILD OFF CACHE BOOL "" FORCE)
  else()
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
  # stb_image_write (demo PNG screenshots). stb is tagless: pin a commit.
  # stb ships no CMakeLists, so populate without add_subdirectory.
  FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20
    GIT_SHALLOW    TRUE
  )
  FetchContent_GetProperties(stb)
  if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
  endif()

  add_executable(tiles_demo samples/demo/main.cpp)
  target_link_libraries(tiles_demo PRIVATE tiles_renderer SDL3::SDL3-static)
  target_include_directories(tiles_demo PRIVATE "${stb_SOURCE_DIR}")
  target_compile_definitions(tiles_demo PRIVATE TILES_WITH_SDL3)
  target_compile_features(tiles_demo PRIVATE cxx_std_20)
  # P3: ship the tiny vendored tileset next to the demo binary so the demo
  # runs a real tileset by default (see --tileset / --no-tileset).
  add_custom_command(TARGET tiles_demo POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/p3_box_tileset"
            "$<TARGET_FILE_DIR:tiles_demo>/p3_box_tileset"
    COMMENT "Copying p3_box_tileset next to tiles_demo")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

message(STATUS "[P0] Configured tiles_renderer ${PROJECT_VERSION} for platform '${TILES_PLATFORM}'")
