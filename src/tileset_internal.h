#pragma once
// tiles_renderer SDK — internal tileset integration (P3).
//
// Not part of the public API. Owns a Cesium3DTilesSelection::Tileset and
// turns loaded tile content into Filament renderables via gltfio.
//
// Design notes (see docs/adr/0005-p3-tileset-rendering.md):
//  - Tile *selection*/LOD is cesium-native's job (updateViewGroup).
//  - Tile *content* (glb bytes) is handed to gltfio's AssetLoader in
//    prepareInMainThread; the raw bytes come from TileLoadResult.
//  - Visibility is toggled per frame from Tile::isRenderable(), so only the
//    tiles selected by the current traversal are in the Filament scene.
//  - The vendored test tileset uses local (non-geospatial) coordinates, so
//    no ENU transform is needed. Real geospatial tilesets will need one
//    (TODO P4).

#include <memory>
#include <string>
#include <vector>

// P33-fix: no glm in this header. Windows/Android/iOS CI build the SDK
// WITHOUT cesium-native (P22 rule), so a <glm/...> include here breaks
// those three platforms. The model matrix crosses this interface as a
// plain double[16] (column-major, same as the public API); the glm
// conversion lives in tileset.cpp inside the TILES_WITH_CESIUM_NATIVE
// guard.

#include "tiles_renderer/renderer.h" // P17: TileStats

namespace filament {
class Engine;
class Scene;
} // namespace filament

namespace tiles_renderer {

struct OrbitCamera {
    float yawDegrees = 0.0f;
    float pitchDegrees = 20.0f;
    float distance = 28.0f;
    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetZ = 0.0f;
};

// P37-C1: explicit lookAt camera (for cesium.js rendering conformance).
// When enabled via Renderer::setCamera(), the orbit camera is bypassed.
struct ExplicitCamera {
    bool enabled = false;
    double eye[3] = {0.0, 0.0, 4.0};
    double target[3] = {0.0, 0.0, 0.0};
    double up[3] = {0.0, 1.0, 0.0};
};

// All methods run on the main/render thread.
class TilesetRenderer {
public:
    TilesetRenderer(filament::Engine* engine, filament::Scene* scene);
    ~TilesetRenderer();

    TilesetRenderer(const TilesetRenderer&) = delete;
    TilesetRenderer& operator=(const TilesetRenderer&) = delete;

    // Load tileset.json from a local filesystem path or file:// URL.
    // Synchronous from the caller's perspective for local files (the load
    // itself is async internally; update() pumps it).
    bool load(const std::string& urlOrPath);

    // P31: load with explicit options (see Renderer::TilesetOptions).
    bool load(const std::string& urlOrPath,
              const Renderer::TilesetOptions& options);

    // P12: why the last load() failed (empty when it succeeded).
    std::string lastError() const;

    // Per-frame work: pump async tasks, run tile selection for the orbit
    // camera, toggle tile visibility. Must be called before rendering.
    void update(double viewportWidth, double viewportHeight,
                const OrbitCamera& camera);

    bool isLoaded() const;

    // Number of tiles selected for rendering by the last update()
    // (-1 when no tileset is loaded).
    int renderedTileCount() const;

    // P17: streaming diagnostics snapshot (see renderer.h TileStats).
    // All fields are -1 when no tileset is loaded.
    Renderer::TileStats tileStats() const;

    // P20: ID strings of tilesToRenderThisFrame from the last update()
    // (diagnostic for frustum/LOD tests). Empty when no tileset is loaded.
    std::vector<std::string> selectedTileIds() const;

    // P19: tile cache budget in bytes. Applies to the next loadTileset()
    // (via TilesetOptions) and live to an already-loaded tileset (via
    // Tileset::getOptions()); values <= 0 restore the cesium-native default
    // (512MB). Must be called on the render thread.
    void setMaxCachedBytes(std::int64_t bytes);

    // P31: live LOD screen-space error budget. Stashed for the next load()
    // and forwarded to a loaded tileset via Tileset::getOptions() (takes
    // effect on the next frame; no reload needed). Values < 0 restore the
    // default of 16. Must be called on the render thread.
    void setMaximumScreenSpaceError(double sse);

    // P31: effective maximumScreenSpaceError (live tileset's value, else the
    // pending setMaximumScreenSpaceError() value, else the default 16).
    double maximumScreenSpaceError() const;

    // P31: options the currently loaded tileset was constructed with
    // (diagnostic). Default options when no tileset is loaded.
    Renderer::TilesetOptions currentOptions() const;

    // P32: event callbacks (see Renderer::TilesetEventCallbacks). Stored per
    // TilesetRenderer; Renderer::setEventCallbacks forwards to the live one.
    // Must be called on the render thread.
    void setEventCallbacks(const Renderer::TilesetEventCallbacks& callbacks);

    // P32: synchronously fire onTileUnload for every tile whose content is
    // currently loaded (last observed state Done). Called by
    // Renderer::loadTileset before a successful load replaces this
    // TilesetRenderer. Must be called on the render thread.
    void fireTileUnloadEvents();

    // P33: show / preloadWhenHidden / modelMatrix (see renderer.h). The
    // show/preload flags and the matrix are stashed per TilesetRenderer;
    // Renderer::set* forwards to the live one (or stashes pre-load).
    // setModelMatrix re-applies the transform to already-loaded tiles
    // immediately. Must be called on the render thread.
    void setShow(bool show);
    bool isShow() const;
    void setPreloadWhenHidden(bool preload);
    bool isPreloadWhenHidden() const;
    void setModelMatrix(const double matrix[16]);
    void modelMatrix(double out[16]) const;

    // P33: read-only tileset state (see renderer.h). Defaults when no
    // tileset is loaded. Must be called on the render thread.
    bool tilesLoaded() const;
    Renderer::BoundingSphere boundingSphere() const;
    std::int64_t timeSinceLoadMs() const;
    std::string rootTileId() const;

    // P34: cache / statistics (see renderer.h). Must be called on the
    // render thread.
    std::int64_t totalMemoryUsageInBytes() const;
    void trimLoadedTiles();
    bool hasExtension(const std::string& name) const;

    // P35: debug switches (see renderer.h). P36: split into tile /
    // content / request volumes + freeze frame. Stashed per
    // TilesetRenderer; Renderer::set* forwards to the live one (or stashes
    // pre-load). Must be called on the render thread.
    void setDebugShowBoundingVolume(bool show);
    bool isDebugShowBoundingVolume() const;
    void setDebugShowContentBoundingVolume(bool show);
    bool isDebugShowContentBoundingVolume() const;
    void setDebugShowViewerRequestVolume(bool show);
    bool isDebugShowViewerRequestVolume() const;
    void setDebugFreezeFrame(bool freeze);
    bool isDebugFreezeFrame() const;
    void setDebugShowUrl(bool show);
    bool isDebugShowUrl() const;

    // P37-C1: P5 rebase origin (world coordinates, double). The explicit
    // camera set via Renderer::setCamera() is in world space; subtract this
    // before passing to Filament (tiles are rendered rebased). Defaults to
    // (0,0,0) = no rebase. Must be called on the render thread.
    void localOrigin(double out[3]) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace tiles_renderer
