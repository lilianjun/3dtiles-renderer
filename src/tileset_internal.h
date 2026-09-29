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

    // P19: tile cache budget in bytes. Applies to the next loadTileset()
    // (via TilesetOptions) and live to an already-loaded tileset (via
    // Tileset::getOptions()); values <= 0 restore the cesium-native default
    // (512MB). Must be called on the render thread.
    void setMaxCachedBytes(std::int64_t bytes);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace tiles_renderer
