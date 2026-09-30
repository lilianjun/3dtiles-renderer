// MinGW-w64 cross-compile shim (windows-mingw preset only).
//
// Filament's utils/compiler.h does `#include <Basetsd.h>` (capital B) under
// WIN32. That resolves on case-insensitive Windows filesystems, but not on
// Linux, where MinGW-w64 ships basetsd.h (lowercase). This shim forwards to
// the real header. It is only on the include path for the windows-mingw
// compile gate (see cmake/toolchain-windows-mingw64.cmake), never for real
// MSVC builds.
#include <basetsd.h>
