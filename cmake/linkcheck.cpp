// P4: link-check shim for cross platforms (Android/iOS).
//
// These targets build no demo/tests, so nothing would ever *link* the SDK
// static library — a broken prebuilt lib list (wrong path, missing archive)
// would go unnoticed. This shared library forces the linker to resolve the
// SDK's Filament references, validating the per-platform prebuilt wiring.
//
// It is never installed or shipped; it exists only to fail the build loudly
// when the Filament prebuilt wiring is wrong.
#include "tiles_renderer/renderer.h"

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

namespace {
// Touch the SDK API so the linker pulls in the SDK objects (and, through
// them, the Filament archives).
const char* touch_sdk() {
    return tiles_renderer::Renderer::version();
}
} // namespace

#if defined(__ANDROID__)
// Also touch the ANativeWindow type used by the Android swap-chain path.
const void* touch_native_window_type(const ANativeWindow* w) {
    return static_cast<const void*>(w);
}
#endif

extern "C" const char* tiles_renderer_linkcheck() {
    return touch_sdk();
}
