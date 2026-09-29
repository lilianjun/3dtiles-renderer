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

    // P3: load a 3D Tiles tileset (local filesystem path, file:// URL, or
    // P5: http(s):// URL — served by the SDK's NonThrowingCurlAccessor;
    // P18: network failures surface as synthetic HTTP 599, never as C++
    // exceptions (mixed libstdc++/libc++ runtimes make those unsafe).
    // Must be called after initialize(). Tile selection/LOD runs every frame
    // in renderFrame() against the orbit camera set via setOrbitCamera().
    // Returns false when not initialized or the tileset failed to load
    // (see lastError() for why). Loading a new tileset replaces the old one;
    // there is no separate unload — dropping the tileset happens via
    // shutdown() or by loading another one.
    static bool loadTileset(const std::string& tilesetUrl);

    // P3: orbit camera used for tile selection and the Filament view.
    // Only takes effect while a tileset is loaded; otherwise the P2 fixed
    // camera is kept. yaw/pitch in degrees, distance in the tileset's units.
    // The camera orbits the tileset's local origin (P5 rebase); there is no
    // free lookAt/target API in this version — see docs/integration.md.
    static void setOrbitCamera(float yawDegrees, float pitchDegrees,
                               float distance);

    // P12: resize the render surface (host window resize, orientation
    // change, split-screen, ...). The host keeps owning the native window;
    // the SDK recreates its swap chain for the same window handle and
    // updates the viewport + camera aspect. Must be called on the render
    // thread, between frames (not from inside a frame callback).
    // Returns false when not initialized or either dimension is zero.
    // No-op (returns true) when the size is unchanged.
    static bool resize(std::uint32_t width, std::uint32_t height);

    // P12: human-readable description of the most recent SDK failure
    // (initialize / loadTileset / resize returning false). Empty when the
    // last such call succeeded. Valid until the next SDK call; the SDK is
    // single-threaded (see below), so no lifetime hazards beyond that.
    static std::string lastError();

    // P3: number of tiles selected for rendering by the last renderFrame()
    // (-1 when no tileset is loaded).
    static int renderedTileCount();

    // P17: streaming diagnostics. A snapshot of the tile pipeline taken by
    // the most recent renderFrame(). All fields are -1 when no tileset is
    // loaded.
    //
    // Field semantics:
    //   selectedTiles — tiles chosen for rendering by the last traversal
    //                   (ViewUpdateResult::tilesToRenderThisFrame). This can
    //                   be larger than renderedTileCount(): the root tile is
    //                   often selected but has no renderable content of its
    //                   own.
    //   tilesLoading  — tiles with content load still outstanding: the
    //                   traversal's worker + main thread load queues PLUS
    //                   tiles whose content fetch/finalize is in flight
    //                   (TileLoadState::ContentLoading/ContentLoaded).
    //                   P18 refinement: queue lengths alone go to 0 while
    //                   curl requests are still downloading, which made the
    //                   gauge lie during slow loads; counting in-flight
    //                   content states keeps it honest.
    //   tilesLoaded   — tiles whose content finished loading
    //                   (TileLoadState::Done). Counts only truly finished
    //                   tiles, not tiles merely referenced but still loading.
    //   tilesFailed   — tiles that failed to load (Failed or
    //                   FailedTemporarily). Computed by walking the
    //                   instantiated tile tree, so it costs O(known tiles);
    //                   fine for diagnostics, don't call it every frame on a
    //                   huge tileset.
    //   bytesLoaded   — tile + raster content bytes currently held
    //                   (cesium-native getTotalDataBytes). Content bytes, NOT
    //                   a GPU memory estimate (see ADR-0015 for why FPS and
    //                   GPU memory are deliberately not reported).
    struct TileStats {
        std::int64_t selectedTiles = -1;
        std::int64_t tilesLoading = -1;
        std::int64_t tilesLoaded = -1;
        std::int64_t tilesFailed = -1;
        std::int64_t bytesLoaded = -1;
    };

    // P17: current TileStats (see above). Like every Renderer method except
    // version(), must be called on the render thread; it only reads state
    // written by renderFrame() and never mutates the scene.
    static TileStats tileStats();

    // P19: cap the tile content cache at `bytes` bytes (LRU eviction of
    // tiles not needed for the current view). Applies to the next
    // loadTileset() and live to an already-loaded tileset (takes effect
    // on the next renderFrame; no reload needed). Values <= 0 restore the
    // cesium-native default of 512MB. Must be called on the render thread,
    // like every Renderer method except version(). Hosts on memory-tight
    // devices (mobile) should set this to fit their budget.
    static void setMaxCachedBytes(std::int64_t bytes);

    static void shutdown();

    static const char* version();
};

// P12: threading model. Every Renderer method except version() must be
// called on ONE thread — the render thread that called initialize().
// Internally the SDK spawns worker threads for tile I/O and parsing
// (cesium-native task system), but all Filament calls and all public API
// state live on the caller's thread; there is no internal locking on the
// API surface. Do not call renderFrame() (or any other method) from two
// threads concurrently.

} // namespace tiles_renderer
