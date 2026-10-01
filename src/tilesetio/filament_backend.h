#pragma once

// tilesetio Filament backend: converts tilesetio::TileRenderData to
// Filament objects (VertexBuffer, IndexBuffer, Renderable).
// This header may include Filament headers, but NOT cesium-native headers.

#include "core.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <gltfio/MaterialProvider.h>

#include <utils/Entity.h>

#include <vector>

namespace tilesetio {

// Filament-side resources for one tile. Created by FilamentBackend::createTile,
// destroyed by FilamentBackend::destroyTile.
struct FilamentTileResources {
    std::vector<utils::Entity> entities;
    // Owned GPU resources (freed on destroy).
    std::vector<filament::VertexBuffer*> vertexBuffers;
    std::vector<filament::IndexBuffer*> indexBuffers;
    // MaterialInstances created from the provider (must be destroyed).
    std::vector<filament::MaterialInstance*> materialInstances;
};

class FilamentBackend {
public:
    // Create Filament resources for a tile's render data and add to scene.
    // materialProvider is the shared UbershaderProvider (owned by caller).
    // Returns per-tile resources for later destruction.
    FilamentTileResources createTile(
        filament::Engine* engine,
        const TileRenderData& data,
        filament::gltfio::MaterialProvider* materialProvider);

    // Remove entities from scene and free GPU resources.
    void destroyTile(
        filament::Engine* engine,
        filament::Scene* scene,
        FilamentTileResources& resources);
};

} // namespace tilesetio
