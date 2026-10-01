#pragma once
// tiles_renderer SDK — public API (see docs/adr/0003-sdk-no-sdl.md).
//
// The SDK is a static library with ZERO SDL dependency. The host app owns the
// window and its lifecycle; it hands the SDK a platform-native window handle
// plus the surface size, and the SDK owns rendering from there.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tiles_renderer {

// Platform-native window handle. The SDK never creates a window itself.
#if defined(__ANDROID__)
struct ANativeWindow;
using NativeWindowHandle = ANativeWindow*;
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

    // P31: tileset load options, aligned with the cesium.js Cesium3DTileset
    // constructor options that have real cesium-native semantics (see
    // docs/tileset-api-roadmap.md). Only options with a direct mapping are
    // exposed; cesium.js-only traversal strategies (skipLevelOfDetail,
    // dynamicScreenSpaceError, foveated*, progressiveResolutionHeightFraction)
    // are deliberately not replicated.
    //
    // Validation at loadTileset(): negative/NaN maximumScreenSpaceError
    // restores 16; 0 maximumSimultaneousTileLoads / loadingDescendantLimit
    // restore 20 (0 simultaneous loads would deadlock tile loading);
    // non-positive/NaN lodTransitionLength restores 1.0. Invalid
    // ellipsoidRadii (non-finite or <= 0) fail the load with lastError —
    // they are not silently replaced.
    struct TilesetOptions {
        // LOD driver: tiles refine while their screen-space error exceeds
        // this (pixels). Smaller = finer detail, more tiles. cesium.js
        // default 16. Live-mutable via setMaximumScreenSpaceError().
        double maximumScreenSpaceError = 16.0;
        // Never render with holes: refuse to refine a parent until all its
        // children are ready. Slower loads, no blank spots while moving.
        bool forbidHoles = false;
        // Preload ancestors/siblings of rendered tiles (better zoom-out/pan
        // at the cost of more tile loads).
        bool preloadAncestors = true;
        bool preloadSiblings = true;
        // Tile culling stages.
        bool enableFrustumCulling = true;
        bool enableFogCulling = true;
        // Tile loading pipeline tuning.
        std::uint32_t maximumSimultaneousTileLoads = 20;
        std::uint32_t loadingDescendantLimit = 20;
        // Smooth LOD transitions (fade between detail levels over
        // lodTransitionLength seconds). Off by default.
        bool enableLodTransitionPeriod = false;
        float lodTransitionLength = 1.0f;
        // Reference ellipsoid radii in meters (WGS84 default). Only used for
        // geospatial tilesets.
        double ellipsoidRadii[3] = {6378137.0, 6378137.0, 6356752.3142451793};
    };

    // P31: loadTileset with explicit options (see TilesetOptions). The
    // single-argument overload uses default options. Options apply at
    // construction; changing them requires reloading (except
    // maximumScreenSpaceError, which is live-mutable via
    // setMaximumScreenSpaceError()). Like the plain overload, loading a new
    // tileset replaces the old one, and a failed load leaves the current
    // tileset untouched.
    static bool loadTileset(const std::string& tilesetUrl,
                            const TilesetOptions& options);

    // P31: change the LOD screen-space error budget live (takes effect on
    // the next renderFrame; no reload needed). Applies to the next
    // loadTileset() when called before any tileset is loaded. Values < 0
    // (or NaN) restore the default of 16. Must be called on the render
    // thread.
    static void setMaximumScreenSpaceError(double sse);

    // P31: the effective maximumScreenSpaceError: the live tileset's value
    // when one is loaded, else a pending setMaximumScreenSpaceError() value,
    // else the default 16.
    static double maximumScreenSpaceError();

    // P31: the options the currently loaded tileset was constructed with
    // (diagnostic; e.g. verifying what a host passed). Default options when
    // no tileset is loaded.
    static TilesetOptions currentTilesetOptions();

    // P32: payload for per-tile events. tileId uses the same string form as
    // selectedTileIds(). url is the tile content URL when the tile's ID
    // carries one (external tileset references), otherwise empty.
    struct TileEventInfo {
        std::string tileId;
        std::string url;
    };

    // P32: payload for onTileFailed. tileId/url as in TileEventInfo; message
    // is the failure reason ("tile content failed to load" for per-tile
    // content failures, since cesium-native does not surface the underlying
    // error text on the Tile; the tileset.json loader's message otherwise).
    struct TileFailedInfo {
        std::string tileId;
        std::string url;
        std::string message;
    };

    // P32: cesium.js-style tileset event callbacks (7 events). All callbacks
    // are invoked on the render thread, inside renderFrame(), in a
    // deterministic order per frame:
    //   tileLoad/tileUnload/tileFailed (state transitions), tileVisible
    //   (render selection), loadProgress (when pending/processing counts
    //   change), allTilesLoaded (every frame the view is fully loaded),
    //   initialTilesLoaded (once per loadTileset).
    // Timing notes (honest differences from cesium.js):
    // - tileLoad fires when a tile's content becomes renderable (state ->
    //   Done), not during traversal; tileUnload when its content is released.
    // - A loadTileset() that replaces a loaded tileset fires tileUnload for
    //   the old tileset's loaded tiles synchronously inside loadTileset().
    // - Callbacks registered via setEventCallbacks() persist across
    //   loadTileset() calls until clearEventCallbacks().
    // - Do NOT call mutating Renderer APIs (loadTileset, set*, shutdown)
    //   from inside a callback; query APIs (tileStats, selectedTileIds,
    //   maximumScreenSpaceError) are safe.
    // - Callbacks must not throw: an exception escaping into the render
    //   loop is undefined behavior (the SDK does not catch across the
    //   callback boundary). Handle errors inside the callback.
    // - Per-tile transition events (tileLoad/tileUnload/tileFailed) are
    //   collected during the frame's state walk and dispatched after the
    //   walk completes, so a callback never observes mid-walk state.
    struct TilesetEventCallbacks {
        std::function<void(const TileEventInfo&)> onTileLoad;
        std::function<void(const TileEventInfo&)> onTileUnload;
        std::function<void(const TileFailedInfo&)> onTileFailed;
        std::function<void(const TileEventInfo&)> onTileVisible;
        std::function<void()> onAllTilesLoaded;
        std::function<void(std::int64_t pendingRequests,
                           std::int64_t tilesProcessing)>
            onLoadProgress;
        std::function<void()> onInitialTilesLoaded;
    };

    // P32: register event callbacks (applies to the next loadTileset() when
    // called before any tileset is loaded, and live when one is). Must be
    // called on the render thread.
    static void setEventCallbacks(const TilesetEventCallbacks& callbacks);

    // P32: remove all event callbacks. Must be called on the render thread.
    static void clearEventCallbacks();

    // P33: cesium.js-style show / modelMatrix / preloadWhenHidden, plus
    // read-only tileset state. All must be called on the render thread.
    //
    // show (default true): whether the tileset is rendered. When false, no
    // tile content is submitted to the Filament scene. preloadWhenHidden
    // (default false) decides what else happens while hidden: when true,
    // the traversal keeps running so tiles keep loading (but never
    // render); when false, the traversal is skipped entirely and the
    // tileset is frozen.
    //
    // modelMatrix (default identity, column-major 4x4): transforms the
    // whole tileset in world space for rendering. It applies to already
    // loaded tiles immediately and to tiles loaded later. The rebase
    // origin (P5) is a fixed float32-precision device and does NOT move
    // with the matrix, so the tileset visibly moves relative to the
    // orbit camera target — the cesium.js behavior. Honest difference
    // from cesium.js: tile selection and LOD still use the tileset's
    // authored (untransformed) tile transforms, so for large placements
    // prefer setting the matrix before (or right after) loadTileset.
    //
    // Read-only state (defaults when no tileset is loaded):
    // - tilesLoaded(): every tile needed for the current view is loaded
    //   (same condition as P32's allTilesLoaded, but queryable any time).
    // - boundingSphere(): the tileset's bounding sphere in world space,
    //   with modelMatrix applied (center + radius; radius uses the
    //   matrix's maximum axis scale).
    // - timeSinceLoadMs(): ms since the tileset was loaded and first
    //   updated; 0 when no tileset is loaded or no frame ran yet.
    // - rootTileId(): the tileset.json root tile's ID (cesium-native's
    //   internal empty-ID wrapper tile is unwrapped); "" when none.
    static void setShow(bool show);
    static bool isShow();
    static void setPreloadWhenHidden(bool preload);
    static bool isPreloadWhenHidden();
    static void setModelMatrix(const double matrix[16]);
    static void modelMatrix(double out[16]);
    static bool tilesLoaded();
    struct BoundingSphere {
        double center[3];
        double radius;
    };
    static BoundingSphere boundingSphere();
    static std::int64_t timeSinceLoadMs();
    static std::string rootTileId();

    // P35: debug switches (subset of cesium.js debug*). P36: split into
    // tile / content / request volumes to match the Cesium3DTilesInspector
    // Display section 1:1. All must be called on the render thread. Values
    // are stashed and forwarded across loadTileset() like the P33 display
    // flags.
    //
    // - setDebugShowBoundingVolume(true): draws each *tile's* bounding
    //   volume from tileset.json as line boxes (box volumes exact; sphere
    //   approximated by its box; region volumes skipped).
    // - setDebugShowContentBoundingVolume(true): draws each loaded tile's
    //   content bounding-box hierarchy as lines (via
    //   FilamentAsset::getWireframe). This is what P35's
    //   setDebugShowBoundingVolume drew; the flag was split in P36 so the
    //   Inspector's Bounding/Content volume checkboxes map 1:1.
    // - setDebugShowViewerRequestVolume(true): draws each tile's
    //   viewerRequestVolume (when the tileset declares one) as line boxes.
    // - setDebugShowUrl(true): logs the IDs of tiles as they become
    //   visible to stderr (no on-screen text renderer in this SDK).
    // - setDebugFreezeFrame(true): skips the tile selection/LOD update;
    //   the last frame's tiles keep rendering (cesium.js debugFreezeFrame).
    //
    // NOT provided: debugWireframe — Filament v1.77 has no runtime
    // wireframe toggle for gltfio materials (rasterization mode is baked
    // at material build time). See ADR-0034.
    static void setDebugShowBoundingVolume(bool show);
    static bool isDebugShowBoundingVolume();
    static void setDebugShowContentBoundingVolume(bool show);
    static bool isDebugShowContentBoundingVolume();
    static void setDebugShowViewerRequestVolume(bool show);
    static bool isDebugShowViewerRequestVolume();
    static void setDebugShowUrl(bool show);
    static bool isDebugShowUrl();
    static void setDebugFreezeFrame(bool freeze);
    static bool isDebugFreezeFrame();

    // P36: overlay hook for dev-tool UIs (e.g. the Inspector panel in
    // tiles_demo --inspector). The callback runs on the render thread
    // inside renderFrame(), after the 3D view is rendered and before the
    // frame is presented; it also runs inside readPixels() so screenshots
    // capture the overlay. Not set by default (zero overhead). The demo
    // uses nativeEngineHandle() to build its overlay with the Filament API.
    using OverlayCallback = std::function<void()>;
    static void setOverlayCallback(OverlayCallback cb);
    // Opaque handle to the underlying filament::Engine (nullptr when not
    // initialized or when built without Filament). For dev-tool use only.
    static void* nativeEngineHandle();
    // Opaque handle to the underlying filament::Renderer (nullptr when not
    // initialized or when built without Filament). The overlay callback
    // uses this to render its own UI view inside the frame. For dev-tool
    // use only.
    static void* nativeRendererHandle();

    // P34: cesium.js-style cache / statistics / method alignment. All must
    // be called on the render thread.
    //
    // - totalMemoryUsageInBytes(): tile + raster content bytes currently
    //   held (cesium-native getTotalDataBytes). Content bytes, NOT a GPU
    //   memory estimate — the same value as TileStats::bytesLoaded,
    //   queryable any time. 0 when no tileset is loaded.
    // - trimLoadedTiles(): unloads tiles not needed for the current view,
    //   freeing their content bytes. Takes effect on the next renderFrame:
    //   the cache budget is briefly set to 0 so cesium-native's
    //   unloadCachedBytes evicts everything not in use, then restored.
    //   Tiles in use (visible this frame) are never unloaded. Fires
    //   tileUnload events through the P32 callback path.
    // - hasExtension(name): whether the loaded tileset.json declared
    //   `name` in its top-level "extensionsUsed". Cached at loadTileset
    //   time; false when no tileset is loaded.
    // - loadTilesetAsync: deliberately NOT provided. The SDK is a
    //   single-threaded render model (every method except version() must
    //   run on the render thread); a true async tileset constructor would
    //   need a full thread-safety rework of Impl and the Filament
    //   resources. Hosts that need non-blocking loads should call
    //   loadTileset on a worker thread and marshal renderFrame to the
    //   render thread.
    static std::int64_t totalMemoryUsageInBytes();
    static void trimLoadedTiles();
    static bool hasExtension(const std::string& name);

    // P3: orbit camera used for tile selection and the Filament view.
    // Only takes effect while a tileset is loaded; otherwise the P2 fixed
    // camera is kept. yaw/pitch in degrees, distance in the tileset's units.
    // The camera orbits the tileset's local origin (P5 rebase); there is no
    // free lookAt/target API in this version — see docs/integration.md.
    static void setOrbitCamera(float yawDegrees, float pitchDegrees,
                               float distance);

    // P37-C1: explicit lookAt camera (for cesium.js rendering conformance).
    // Overrides the orbit camera; tile selection uses this camera's
    // position. eye/target/up in the tileset's local coordinates
    // (after P5 rebase). Call clearExplicitCamera() to revert to orbit.
    static void setCamera(const double eye[3], const double target[3],
                          const double up[3]);
    static void clearExplicitCamera();
    static bool hasExplicitCamera();

    // P37-C1: background clear color (rgba 0-1, default dark blue) and
    // vertical FOV in degrees (default 60). For cesium.js conformance.
    static void setClearColor(float r, float g, float b, float a);
    static void setFovDegrees(float fovDegrees);

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
        // P36: Inspector statistics section.
        //   tilesVisited    — tiles in the instantiated tree (walked)
        //   pendingRequests — tiles queued for load (worker + main queues)
        //   tilesProcessing — tiles with content in flight
        //                     (ContentLoading/ContentLoaded)
        // tilesLoading == pendingRequests + tilesProcessing.
        std::int64_t tilesVisited = -1;
        std::int64_t pendingRequests = -1;
        std::int64_t tilesProcessing = -1;
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

    // P20: ID strings of the tiles chosen for rendering by the most recent
    // renderFrame() (ViewUpdateResult::tilesToRenderThisFrame, via
    // cesium-native TileIdUtilities::createTileIdString). Diagnostic for
    // frustum-culling / LOD tests and host debugging; the string format is
    // cesium-native's and not contractual. Empty when no tileset is loaded.
    // Must be called on the render thread, like tileStats().
    static std::vector<std::string> selectedTileIds();

    // P26: toggle the default image-based lighting (procedural environment,
    // see docs/adr/ADR-0025.md). On by default; pass false to render with
    // the P3 directional sun only (pre-P26 look). Takes effect immediately
    // (next renderFrame) and is cheap: the environment is built once at
    // initialize() and toggling only attaches/detaches the IndirectLight
    // from the scene. The intensity is fixed at Filament's default (30000);
    // there is intentionally no brightness knob in this version.
    // Must be called on the render thread, like every Renderer method
    // except version(). No-op (returns silently) when IBL could not be
    // built at initialize() or the platform has no Filament backend.
    static void setIblEnabled(bool enabled);

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
