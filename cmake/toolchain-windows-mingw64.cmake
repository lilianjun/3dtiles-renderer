# MinGW-w64 cross toolchain: build Windows (x86_64) objects on a Linux host.
#
# Purpose: fast local *compile gate* for Windows portability (windows.h
# min/max macro hygiene, platform ifdefs, NOMINMAX scoping). It is NOT a
# replacement for the MSVC CI job:
#   - Filament's windows prebuilt .lib files are MSVC-only (C++ ABI); the
#     final link of any executable/shared lib cannot succeed under MinGW.
#   - MSVC-only strictness (/WX, template quirks) is not reproduced.
# Build the `tiles_renderer` static-library target only: archiving objects
# never touches the MSVC .lib files, so header + source portability of our
# code, the Filament windows headers, and cesium-native (via vcpkg
# x64-mingw-static) is fully validated.
#
# Usage:
#   VCPKG_CHAINLOAD_TOOLCHAIN_FILE=$PWD/cmake/toolchain-windows-mingw64.cmake \
#     cmake --preset windows-mingw -DTILES_WITH_FILAMENT=ON \
#       -DTILES_WITH_CESIUM_NATIVE=ON
#   cmake --build --preset windows-mingw --target tiles_renderer
# (The preset already sets VCPKG_CHAINLOAD_TOOLCHAIN_FILE in its
# environment block; the explicit form above is for manual configure lines.)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# -posix thread model: std::thread / std::mutex must work (cesium-native
# spawns worker threads). The -win32 variant cannot compile <thread>.
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Case-sensitivity shims (see cmake/mingw-shims/): Filament's headers
# include <Basetsd.h> with a capital B, which MinGW-w64 on Linux cannot
# resolve. Only used by this compile gate, never by real MSVC builds.
include_directories("${CMAKE_CURRENT_LIST_DIR}/mingw-shims")

# MinGW-w64 hides M_PI etc. in <math.h> under strict ISO (-std=c++17);
# cesium-native (via s2geometry headers) needs them visible. Applied here
# so it covers both vcpkg port builds and the main build. The overlay
# triplet in cmake/vcpkg-triplets carries the same flag for ports.
add_definitions(-D_GNU_SOURCE)
