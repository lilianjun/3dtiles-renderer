cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# Target platform selection.
# Values: linux | windows | android | ios
# Declared before project() so the cross-toolchain wiring below can read it.
# ---------------------------------------------------------------------------
set(TILES_PLATFORM "linux" CACHE STRING "Target platform")
set_property(CACHE TILES_PLATFORM PROPERTY STRINGS linux windows android ios)

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
      # P31-fix: android-28 is the minimum that links cesium-native's vcpkg
      # deps: vcpkg builds spdlog/curl against >= 28 headers
      # (fwrite_unlocked entered libc at API 28); linking those archives
      # against the android-24 platform stub fails with undefined symbols.
      set(ANDROID_PLATFORM "android-28" CACHE STRING "Android API level")
      set(ANDROID_STL "c++_shared" CACHE STRING "Android STL")
      set(TILES_CROSS_TOOLCHAIN "android-ndk")
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

# Windows: keep windows.h from #defining min/max macros. Without NOMINMAX,
# any TU that (transitively) includes both windows.h and a header using
# std::min/std::max (e.g. filament's utils/bitset.h) fails with
# C2589 "'(' : illegal token on right side of '::'", plus cascading bogus
# errors. WIN32_LEAN_AND_MEAN keeps the header light as a bonus.
# NOTE: scoped to OUR targets only via TILES_WINDOWS_DEFS — a global
# add_compile_definitions would leak into cesium-native's own targets
# (CesiumCurl #defines NOMINMAX itself -> C4005 redefinition, /WX error).
if(TILES_PLATFORM STREQUAL "windows")
  set(TILES_WINDOWS_DEFS NOMINMAX WIN32_LEAN_AND_MEAN)
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

# ---------------------------------------------------------------------------
# P6: sanitizer builds (ASan + LSan + UBSan) for OUR targets only.
# Third-party deps (cesium-native, SDL3, Filament prebuilts) are intentionally
# NOT instrumented: they are prebuilt or very expensive to rebuild, and the
# sanitizer runtimes still intercept their allocations at run time, so leaks
# in third-party code are still *detected* (triaged via tests/lsan.supp).
# Usage: -DTILES_SANITIZE=ON, or the linux-asan preset. See docs/adr/0007.
# ---------------------------------------------------------------------------
option(TILES_SANITIZE "Build our targets with -fsanitize=address,undefined (Linux/GCC/Clang only)" OFF)

function(tiles_apply_sanitizers target)
  if(NOT TILES_SANITIZE)
    return()
  endif()
  if(NOT TILES_PLATFORM STREQUAL "linux")
    message(WARNING "[P6] TILES_SANITIZE is only supported on linux — ignoring for '${target}'")
    return()
  endif()
  if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    message(WARNING "[P6] TILES_SANITIZE needs GCC or Clang — ignoring for '${target}'")
    return()
  endif()
  # -fno-sanitize-recover=all: any UB aborts instead of print-and-continue,
  # so the sanitizer ctest gate cannot miss it. LSan is part of ASan on Linux
  # (detect_leaks defaults to 1); third-party leak triage lives in
  # tests/lsan.supp.
  #
  # -fno-sanitize=vptr: UBSan's vptr check requires RTTI typeinfo in vtables,
  # but the prebuilt Filament and cesium-native static libs are compiled with
  # -fno-rtti (verified: zero __typeinfo symbols in libgltfio_core.a and the
  # cesium archives). Every virtual call through a third-party static type
  # (e.g. MaterialProvider::destroyMaterials()) therefore false-positives as
  # "invalid vptr" even though the object is valid (vptr stable from
  # construction to destruction, ASan-clean). All other UBSan checks stay on.
  target_compile_options(${target} PRIVATE
    -fsanitize=address,undefined -fno-sanitize=vptr
    -fno-omit-frame-pointer
    -fno-sanitize-recover=all -g)
  # Note: for the STATIC tiles_renderer lib these link options are inert at
  # archive time; the sanitizer runtimes get linked via the executables below.
  target_link_options(${target} PRIVATE
    -fsanitize=address,undefined -fno-sanitize=vptr
    -fno-sanitize-recover=all)
endfunction()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# MSVC reports __cplusplus as 199711L unless /Zc:__cplusplus is given —
# our smoke test (and any standard-conformant check) needs the real value.
if(MSVC)
  add_compile_options(/Zc:__cplusplus)
endif()
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
add_library(tiles_renderer STATIC src/renderer.cpp src/tileset.cpp
            src/converter_guard.cpp
            src/tilesetio/cesium_adapter.cpp
            src/tilesetio/filament_backend.cpp)
tiles_apply_sanitizers(tiles_renderer)
if(DEFINED TILES_WINDOWS_DEFS)
  target_compile_definitions(tiles_renderer PRIVATE ${TILES_WINDOWS_DEFS})
endif()
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
  # P13: let cesium-native install its static libs + headers + CMake config
  # into the same prefix as ours, so find_package(tiles_renderer) can
  # find_dependency(cesium-native). These only add install() rules; the
  # build itself is unchanged.
  set(CESIUM_INSTALL_STATIC_LIBS ON CACHE BOOL "" FORCE)
  set(CESIUM_INSTALL_HEADERS ON CACHE BOOL "" FORCE)
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
  # P5: CesiumCurl (libcurl asset accessor) for http(s):// tileset URLs,
  # CesiumGltfWriter to re-serialize converted models (b3dm/i3dm) to glb.
  target_link_libraries(tiles_renderer PUBLIC CesiumCurl CesiumGltfWriter)
  target_compile_definitions(tiles_renderer PRIVATE TILES_WITH_CESIUM_NATIVE)
  # P19-hotfix: src/tileset.cpp includes <glm/...> directly (since P7), but
  # glm only reaches us transitively via cesium-native's glm::glm (through
  # CesiumUtility PUBLIC). That propagation proved unreliable: the vcpkg
  # include dir lands on our compile line on Linux, but was missing on
  # Windows/Android/iOS CI (C1083 / 'glm/gtc/matrix_transform.hpp'
  # file not found), breaking all three. Resolve the headers explicitly from
  # cesium-native's vcpkg tree instead of relying on the transitive link.
  # PRIVATE: our public headers never mention glm.
  if(NOT TILES_GLM_INCLUDE_DIR)
    # ezvcpkg home: $HOME on Unix, %USERPROFILE% on Windows (GitHub Actions
    # sets HOME too, but don't rely on it).
    set(_tiles_ezvcpkg_homes "$ENV{HOME}")
    if(WIN32 AND DEFINED ENV{USERPROFILE})
      list(APPEND _tiles_ezvcpkg_homes "$ENV{USERPROFILE}")
    endif()
    foreach(_tiles_home IN LISTS _tiles_ezvcpkg_homes)
      file(GLOB _tiles_glm_headers "${_tiles_home}/.ezvcpkg/*/installed/*/include/glm/glm.hpp")
      if(_tiles_glm_headers)
        break()
      endif()
    endforeach()
    if(_tiles_glm_headers)
      list(GET _tiles_glm_headers 0 _tiles_glm_hpp)
      get_filename_component(_tiles_glm_dir "${_tiles_glm_hpp}" DIRECTORY)
      get_filename_component(TILES_GLM_INCLUDE_DIR "${_tiles_glm_dir}" DIRECTORY)
      message(STATUS "[P19-hotfix] using glm headers: ${TILES_GLM_INCLUDE_DIR}")
    else()
      message(WARNING "[P19-hotfix] glm headers not found under $ENV{HOME}/.ezvcpkg — "
        "relying on transitive glm::glm (known-broken on non-Linux CI)")
    endif()
    unset(_tiles_glm_headers)
    unset(_tiles_glm_hpp)
    unset(_tiles_glm_dir)
    unset(_tiles_ezvcpkg_homes)
    unset(_tiles_home)
  endif()
  if(TILES_GLM_INCLUDE_DIR)
    target_include_directories(tiles_renderer PRIVATE "${TILES_GLM_INCLUDE_DIR}")
  endif()
endif()

# --- Filament: rendering. Goes into the SDK. ---
# Filament is very expensive to build from source, so we consume the
# official prebuilt binaries for the pinned release (per-platform archives).
# P4 wires linux / android / windows / ios (no Web target — the Emscripten
# stub was removed 2026-09-30; a future web renderer will be a separate JS
# project on filament.js, see ADR-0023 G9).
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
    # bluegl: backend.lib's WGL/OpenGL driver objects (PlatformWGL,
    # OpenGLDriver) reference bluegl_* symbols — it ships as bluegl.lib.
    set(_filament_libs filament backend bluegl bluevk filabridge filaflat
        utils geometry smol-v ibl gltfio gltfio_core uberarchive uberzlib image)
    # MSVC resolves cyclic static deps without --start-group.
    # zstd.lib ships in the package (used by MaterialParser, as on Linux).
    # ws2_32/dbghelp: usual Windows system deps of Filament's utils/backend.
    # opengl32: wgl* functions referenced by backend.lib's PlatformWGL.
    set(_filament_syslibs zstd ws2_32 dbghelp opengl32)
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
      # (OpenGLES: Engine::create()'s backend switch also pulls the GL
      # driver utils into the link, same as GLESv3/EGL on Android.)
      set(_filament_syslibs
          "-framework Foundation" "-framework UIKit" "-framework Metal"
          "-framework CoreGraphics" "-framework CoreVideo"
          "-framework QuartzCore" "-framework IOKit" "-framework CFNetwork"
          "-framework OpenGLES")
    endif()
  endif()

  if(_filament_pkg)
    message(STATUS "[P4] Fetching Filament prebuilt binaries v${TILES_FILAMENT_VERSION} (${_filament_pkg}) ...")
    FetchContent_Declare(
      filament_bin
      URL "${_filament_base_url}/${_filament_pkg}"
    )
    FetchContent_MakeAvailable(filament_bin)
    # P15: gltfio decodes embedded PNG/JPEG textures through a
    # client-supplied TextureProvider; the prebuilt stb decoder (libstb.a)
    # ships in the Filament release archives, so wire it in wherever it
    # exists. Without it, textured materials silently render black
    # ("Missing texture provider for image/png") — unnoticed until P15
    # because no earlier fixture used textures.
    if(_filament_xcframework)
      set(_stb_lib_path
        "${filament_bin_SOURCE_DIR}/lib/libstb.xcframework/ios-arm64/libstb.a")
    elseif(TILES_PLATFORM STREQUAL "windows")
      # Windows prebuilt names it stb.lib (no lib prefix / .a suffix);
      # the old libstb.a-only check silently disabled texture decoding.
      set(_stb_lib_path
        "${filament_bin_SOURCE_DIR}/${_filament_libdir}/stb.lib")
    else()
      set(_stb_lib_path
        "${filament_bin_SOURCE_DIR}/${_filament_libdir}/libstb.a")
    endif()
    if(EXISTS "${_stb_lib_path}")
      list(APPEND _filament_libs stb)
      target_compile_definitions(tiles_renderer PRIVATE TILES_WITH_STB_PROVIDER)
    else()
      message(WARNING "[P15] libstb.a not in the Filament package — glTF "
                      "textures will not decode on ${TILES_PLATFORM}.")
    endif()
    # P25: KTX2 (KHR_texture_basisu / image/ktx2) texture decoding.
    #
    # Symbol-collision hazard (verified 2026-09-29, see ADR-0024): Filament's
    # ktxreader is backed by Filament's bundled basisu (libbasis_transcoder.a,
    # namespace basist::), while cesium-native's vcpkg libktx.a vendors its
    # own incompatible basist:: copy. Linking both makes the static linker
    # resolve filament's ktxreader against cesium's basisu -> heap corruption
    # (standalone probe of filament's transcoder is fine; linking cesium's
    # libktx.a first segfaults on the same file).
    #
    # Therefore:
    #  - On cesium builds (Linux): do NOT link ktxreader/basis_transcoder.
    #    KTX2 is decoded by our own provider (src/ktx2_libktx_provider.cpp)
    #    using the same libktx cesium-native already links, so exactly one
    #    basist:: exists in the binary. Defines TILES_WITH_KTX2_LIBKTX.
    #  - Elsewhere (no cesium, hence no libktx, hence no conflict): use
    #    filament's createKtx2Provider via the ktxreader/basis_transcoder
    #    archives, same existence-check pattern as stb. Defines
    #    TILES_WITH_KTX2_PROVIDER.
    if(TILES_WITH_CESIUM_NATIVE)
      target_compile_definitions(tiles_renderer PRIVATE TILES_WITH_KTX2_LIBKTX)
      target_sources(tiles_renderer PRIVATE src/ktx2_libktx_provider.cpp)
      # ktx.h include dir is added after TILES_VCPKG_INSTALLED_DIR is
      # resolved further below.
    else()
      if(_filament_xcframework)
        set(_ktxreader_lib_path
          "${filament_bin_SOURCE_DIR}/lib/libktxreader.xcframework/ios-arm64/libktxreader.a")
        set(_basis_lib_path
          "${filament_bin_SOURCE_DIR}/lib/libbasis_transcoder.xcframework/ios-arm64/libbasis_transcoder.a")
      else()
        set(_ktxreader_lib_path
          "${filament_bin_SOURCE_DIR}/${_filament_libdir}/libktxreader.a")
        set(_basis_lib_path
          "${filament_bin_SOURCE_DIR}/${_filament_libdir}/libbasis_transcoder.a")
      endif()
      if(EXISTS "${_ktxreader_lib_path}" AND EXISTS "${_basis_lib_path}")
        # ktxreader references basis_transcoder: keep this order for linkers
        # without --start-group (iOS full-path layout).
        list(APPEND _filament_libs ktxreader basis_transcoder)
        target_compile_definitions(tiles_renderer PRIVATE TILES_WITH_KTX2_PROVIDER)
      else()
        message(WARNING "[P25] libktxreader.a / libbasis_transcoder.a not in "
                        "the Filament package — KTX2 textures will render "
                        "black on ${TILES_PLATFORM}.")
      endif()
    endif()
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
    # T3 (tilesetio): textured unlit material.
    set(TILES_TEX_MAT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/materials/unlit_textured.mat")
    set(TILES_TEX_FILAMAT "${CMAKE_CURRENT_BINARY_DIR}/generated/unlit_textured.filamat")
    set(TILES_TEX_FILAMAT_H
        "${CMAKE_CURRENT_BINARY_DIR}/generated/unlit_textured_filamat.h")
    add_custom_command(
      OUTPUT "${TILES_TEX_FILAMAT}"
      COMMAND "${TILES_MATC}" -a all -o "${TILES_TEX_FILAMAT}" "${TILES_TEX_MAT_SRC}"
      DEPENDS "${TILES_TEX_MAT_SRC}"
      COMMENT "matc: compiling unlit_textured.mat (all backends)")
    add_custom_command(
      OUTPUT "${TILES_TEX_FILAMAT_H}"
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_filamat.py"
              "${TILES_TEX_FILAMAT}" "${TILES_TEX_FILAMAT_H}" unlit_textured_filamat
      DEPENDS "${TILES_TEX_FILAMAT}"
              "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_filamat.py"
      COMMENT "Embedding unlit_textured.filamat as C++ header")
    add_custom_target(tiles_materials DEPENDS "${TILES_FILAMAT_H}" "${TILES_TEX_FILAMAT_H}")
    add_dependencies(tiles_renderer tiles_materials)

    # P36-B pivot: the Filament ImGui backend was abandoned (see ADR-0035);
    # the UI material is no longer needed. The Inspector uses a separate
    # SDL3 window with imgui_impl_sdlrenderer3.
    # P4: cross platforms build no demo/tests, so nothing would ever link
    # the SDK — add a shared-lib link check to validate the prebuilt lib
    # list (paths + symbol resolution) on each wired platform.
    if(TILES_PLATFORM STREQUAL "android" OR TILES_PLATFORM STREQUAL "ios")
      add_library(tiles_renderer_linkcheck SHARED cmake/linkcheck.cpp)
      target_link_libraries(tiles_renderer_linkcheck PRIVATE tiles_renderer)
      # Android on-device smoke harness (ADR-0023 G1): libsmoke_jni.so links
      # the real SDK exactly like a host app would; the APK in
      # samples/android-smoke consumes it as a prebuilt jniLib (Gradle does
      # no NDK compile). Guarded on the real NDK toolchain so the host-stub
      # android preset (P0 policy) keeps configuring without <jni.h>.
      if(TILES_PLATFORM STREQUAL "android"
          AND TILES_CROSS_TOOLCHAIN STREQUAL "android-ndk")
        add_library(smoke_jni SHARED samples/android-smoke/jni/smoke_jni.cpp)
        target_link_libraries(smoke_jni PRIVATE tiles_renderer log android)
      endif()
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

  # P36-B: Dear ImGui (demo-only UI for the Inspector panel; the SDK
  # stays zero-ImGui). Pin a release tag.
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.2b
    GIT_SHALLOW    TRUE
  )
  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
  endif()
  # ImGui ships no CMakeLists: build the core + SDL3 backend as a
  # demo-private static lib. The Filament renderer backend is our own
  # (samples/demo/imgui_filament_backend.cpp).
  add_library(imgui_demo STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
  )
  target_include_directories(imgui_demo PUBLIC
    "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
  target_link_libraries(imgui_demo PUBLIC SDL3::SDL3-static)
  target_compile_features(imgui_demo PRIVATE cxx_std_20)

  add_executable(tiles_demo samples/demo/main.cpp)
  tiles_apply_sanitizers(tiles_demo)
  if(DEFINED TILES_WINDOWS_DEFS)
    target_compile_definitions(tiles_demo PRIVATE ${TILES_WINDOWS_DEFS})
  endif()
  target_link_libraries(tiles_demo PRIVATE tiles_renderer SDL3::SDL3-static imgui_demo)
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

# ---------------------------------------------------------------------------
# P13: installable SDK.
#
# Installs:
#   <prefix>/lib[/<multiarch>]/libtiles_renderer.a
#   <prefix>/include/tiles_renderer/{renderer.h,version.h}
#   <prefix>/lib[/<multiarch>]/cmake/tiles_renderer/tiles_rendererConfig{,Version}.cmake
# plus, when wired in, cesium-native's own install (libs + headers +
# cesium-nativeConfig.cmake, via CESIUM_INSTALL_* above) and Filament's
# prebuilt archives + headers.
#
# Distribution policy (honest): the SDK is distributed as SOURCE, built
# with pinned third-party versions fetched at configure time
# (docs/adr/0002-toolchain.md). There is no prebuilt binary distribution;
# `cmake --install` from an SDK build tree is the supported way to obtain
# an installable SDK. Consumers then use find_package(tiles_renderer).
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# SDK static library + public headers (hand-written + generated version.h).
install(TARGETS tiles_renderer
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/tiles_renderer"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/tiles_renderer/version.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/tiles_renderer")

# Filament prebuilt archives + headers, installed next to the SDK so the
# installed SDK links standalone. Only archive file NAMES are passed to
# the config template (it prepends ${PACKAGE_PREFIX_DIR} itself); syslibs
# stay as plain names, exactly as in the build-tree link — except zstd,
# which is rewritten to Filament's shipped archive when present (see below).
set(TILES_FILAMENT_LIBNAMES "")
set(TILES_FILAMENT_USE_GROUP "OFF")
set(TILES_FILAMENT_SYSLIBS "")
if(TILES_WITH_FILAMENT AND _filament_pkg AND filament_bin_SOURCE_DIR)
  foreach(_lib IN LISTS _filament_libs)
    if(_filament_xcframework)
      set(_arc
        "${filament_bin_SOURCE_DIR}/lib/lib${_lib}.xcframework/ios-arm64/lib${_lib}.a")
    else()
      set(_arc
        "${filament_bin_SOURCE_DIR}/${_filament_libdir}/lib${_lib}.a")
    endif()
    install(FILES "${_arc}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    list(APPEND TILES_FILAMENT_LIBNAMES "lib${_lib}.a")
  endforeach()
  if(_filament_use_group)
    set(TILES_FILAMENT_USE_GROUP "ON")
  endif()
  set(TILES_FILAMENT_SYSLIBS "${_filament_syslibs}")
  # zstd: Filament's linux/android packages ship libzstd.a, but it is listed
  # as a bare syslib (-lzstd), which external consumers may not resolve
  # (no zstd on their default link path). Prefer the shipped archive by
  # absolute path when present.
  if(NOT _filament_xcframework)
    set(_zstd_arc "${filament_bin_SOURCE_DIR}/${_filament_libdir}/libzstd.a")
    if(EXISTS "${_zstd_arc}")
      install(FILES "${_zstd_arc}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
      list(APPEND TILES_FILAMENT_LIBNAMES "libzstd.a")
      list(REMOVE_ITEM TILES_FILAMENT_SYSLIBS "zstd")
    endif()
    unset(_zstd_arc)
  endif()
  install(DIRECTORY "${filament_bin_SOURCE_DIR}/include/"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

# --- vcpkg package CMake configs (for cesium-native's find_dependency) ---
# cesium-native installs its own CMake config, which in turn
# find_dependency()s ~17 vcpkg packages in CONFIG mode. Their
# *-config.cmake files live in the vcpkg installed tree and are
# prefix-relative (relocatable), so install the whole share/ dir next to
# our own files; find_package(<pkg> CONFIG) then resolves them under the
# same prefix via the standard <prefix>/share/<pkg>*/ search path.
set(TILES_VCPKG_INSTALLED_DIR "" CACHE PATH
    "Override: vcpkg 'installed' dir holding cesium-native's dep configs (auto-detected from ezvcpkg)")
if(TILES_WITH_CESIUM_NATIVE AND NOT TILES_VCPKG_INSTALLED_DIR)
  set(_want_triplet "")
  if(TILES_PLATFORM STREQUAL "linux")
    set(_want_triplet "x64-linux")
  elseif(TILES_PLATFORM STREQUAL "windows")
    set(_want_triplet "x64-windows")
  elseif(TILES_PLATFORM STREQUAL "android")
    if(ANDROID_ABI STREQUAL "arm64-v8a")
      set(_want_triplet "arm64-android")
    elseif(ANDROID_ABI STREQUAL "x86_64")
      set(_want_triplet "x64-android")
    endif()
  elseif(TILES_PLATFORM STREQUAL "ios")
    set(_want_triplet "arm64-ios")
  endif()
  file(GLOB _vcpkg_roots "$ENV{HOME}/.ezvcpkg/*/installed/*")
  foreach(_r IN LISTS _vcpkg_roots) # pass 1: triplet match
    if(_want_triplet AND _r MATCHES "/${_want_triplet}/$"
        AND EXISTS "${_r}/share/ada/ada-config.cmake")
      set(TILES_VCPKG_INSTALLED_DIR "${_r}")
      break()
    endif()
  endforeach()
  if(NOT TILES_VCPKG_INSTALLED_DIR)
    foreach(_r IN LISTS _vcpkg_roots) # pass 2: any populated vcpkg tree
      if(EXISTS "${_r}/share/ada/ada-config.cmake")
        set(TILES_VCPKG_INSTALLED_DIR "${_r}")
        break()
      endif()
    endforeach()
  endif()
  unset(_vcpkg_roots)
  unset(_want_triplet)
endif()
if(TILES_VCPKG_INSTALLED_DIR)
  install(DIRECTORY "${TILES_VCPKG_INSTALLED_DIR}/share/"
          DESTINATION "${CMAKE_INSTALL_DATADIR}")
  # cesium's own install() rules miss a few headers that its config chain
  # needs — Findmodp_b64.cmake wants modp_b64.h, and httplibConfig.cmake
  # set_and_check()s include/httplib.h. Compensate with an explicit list
  # (verified by scanning the vcpkg configs' set_and_check references).
  set(_tiles_extra_headers
      modp_b64.h modp_stdint.h httplib.h)
  foreach(_h IN LISTS _tiles_extra_headers)
    if(EXISTS "${TILES_VCPKG_INSTALLED_DIR}/include/${_h}")
      install(FILES "${TILES_VCPKG_INSTALLED_DIR}/include/${_h}"
              DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    else()
      message(WARNING "[P13] expected vcpkg header '${_h}' not found under "
        "${TILES_VCPKG_INSTALLED_DIR}/include — consumer find_package may fail")
    endif()
  endforeach()
  unset(_tiles_extra_headers)
elseif(TILES_WITH_CESIUM_NATIVE)
  message(WARNING "[P13] vcpkg installed dir not found — skipping vcpkg CMake "
    "configs; find_package(tiles_renderer) consumers will need cesium-native's "
    "vcpkg dependencies resolvable another way. Set TILES_VCPKG_INSTALLED_DIR "
    "to override.")
endif()

# P25: the libktx-backed KTX2 provider (cesium builds only, see the
# TILES_WITH_KTX2_LIBKTX block in the Filament section) needs ktx.h from
# cesium-native's vcpkg tree.
if(TILES_WITH_CESIUM_NATIVE AND TILES_WITH_FILAMENT)
  if(TILES_VCPKG_INSTALLED_DIR AND EXISTS
      "${TILES_VCPKG_INSTALLED_DIR}/include/ktx.h")
    target_include_directories(tiles_renderer PRIVATE
      "${TILES_VCPKG_INSTALLED_DIR}/include")
  else()
    message(WARNING "[P25] ktx.h not found under "
      "${TILES_VCPKG_INSTALLED_DIR}/include — KTX2 provider cannot build.")
  endif()
endif()

# vcpkg imported targets reference per-config DEBUG library paths, but this
# SDK installs release third-party libs only. Fix up <prefix>/debug at
# install time so the DEBUG imported locations resolve (see
# cmake/tiles_debug_compat.cmake; debug-info fidelity is not claimed).
if(TILES_VCPKG_INSTALLED_DIR)
  install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tiles_debug_compat.cmake")
endif()

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tiles_rendererConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/tiles_rendererConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tiles_renderer")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/generated/tiles_rendererConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/generated/tiles_rendererConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/tiles_rendererConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tiles_renderer")

message(STATUS "[P0] Configured tiles_renderer ${PROJECT_VERSION} for platform '${TILES_PLATFORM}'")
