# Overlay triplet for the windows-mingw compile gate (Linux host -> Windows).
# Same as vcpkg's community x64-mingw-static, plus -D_GNU_SOURCE:
# MinGW-w64 hides M_PI etc. in <math.h> under strict ISO (-std=c++17),
# which breaks the s2geometry port (cesium-native dependency).
# _GNU_SOURCE is benign for the other ports.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_ENV_PASSTHROUGH PATH)

set(VCPKG_CMAKE_SYSTEM_NAME MinGW)

set(VCPKG_C_FLAGS "-D_GNU_SOURCE")
set(VCPKG_CXX_FLAGS "-D_GNU_SOURCE")
