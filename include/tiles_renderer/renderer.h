#pragma once
// tiles_renderer SDK — public API (see docs/adr/0003-sdk-no-sdl.md).
//
// The SDK is a static library with ZERO SDL dependency. The host app owns the
// window and its lifecycle; it hands the SDK a platform-native window handle
// plus the surface size, and the SDK owns rendering from there.

#include <cstdint>
#include <string>
#include <vector>

namespace tiles_renderer {

// Platform-native window handle. The SDK never creates a window itself.
#if defined(__ANDROID__)
struct ANativeWindow;
using NativeWindowHandle = ANativeWindow*;
#elif defined(__EMSCRIPTEN__)
// Web: the <canvas> element selector, e.g. "#canvas".
using NativeWindowHandle = const char*;
#else
// Windows (HWND), Apple (UIView*/NSView*), Linux (X11 Window / wl_surface*):
// all arrive as an opaque pointer. Filament's createSwapChain takes void*
// on these platforms.
using NativeWindowHandle = void*;
#endif

struct RendererConfig {
    NativeWindowHandle window = nullptr; // required: host-provided native window
    std::uint32_t width = 0;             // required: surface width in pixels
    std::uint32_t height = 0;            // required: surface height in pixels
};

class Renderer {
public:
    Renderer() = delete;

    // Initialize the renderer for the given native surface.
    // Returns false when the config is invalid (null window or zero size),
    // or when the platform backend is not implemented yet (see TODOs in
    // src/renderer.cpp).
    //
    // P2: creates the real Filament engine + swap chain on Linux. Other
    // platforms return false with a clear log until their backends land.
    static bool initialize(const RendererConfig& config);

    // Render one frame. Returns false when not initialized or the frame
    // could not be started (e.g. the swap chain is not ready).
    static bool renderFrame();

    // Read back the most recently rendered frame as 8-bit RGBA, top-left
    // origin, row-major. outRgba is resized to width*height*4. Returns false
    // when not initialized or the read-back did not complete in time.
    // Intended for testing/debugging (screenshots), not per-frame use.
    static bool readPixels(std::vector<std::uint8_t>& outRgba,
                           std::uint32_t& outWidth, std::uint32_t& outHeight);

    // P3: load a 3D Tiles tileset (local filesystem path or file:// URL).
    // Must be called after initialize(). Tile selection/LOD runs every frame
    // in renderFrame() against the orbit camera set via setOrbitCamera().
    // Returns false when not initialized or the tileset failed to load.
    static bool loadTileset(const std::string& tilesetUrl);

    // P3: orbit camera used for tile selection and the Filament view.
    // Only takes effect while a tileset is loaded; otherwise the P2 fixed
    // camera is kept. yaw/pitch in degrees, distance in the tileset's units.
    static void setOrbitCamera(float yawDegrees, float pitchDegrees,
                               float distance);

    // P3: number of tiles selected for rendering by the last renderFrame()
    // (-1 when no tileset is loaded).
    static int renderedTileCount();

    static void shutdown();

    static const char* version();
};

} // namespace tiles_renderer
