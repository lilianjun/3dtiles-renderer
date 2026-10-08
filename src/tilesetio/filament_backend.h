#pragma once

// tilesetio Filament backend: converts tilesetio::TileRenderData (raw memory
// data) to Filament objects via standard Filament API.
// This header may include Filament headers, but NOT cesium-native or
// gltfio headers. Pure Filament, no glTF.

#include "core.h"

#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/Scene.h>

#include <utils/Entity.h>

#include <vector>

namespace tilesetio {

// Filament-side resources for one tile.
struct FilamentTileResources {
    std::vector<utils::Entity> entities;
    std::vector<filament::VertexBuffer*> vertexBuffers;
    std::vector<filament::IndexBuffer*> indexBuffers;
    std::vector<filament::MaterialInstance*> materialInstances;
    std::vector<filament::Texture*> textures;
};

class FilamentBackend {
public:
    FilamentBackend() = default;
    ~FilamentBackend() = default;

    // Non-copyable (holds Engine-owned Material).
    FilamentBackend(const FilamentBackend&) = delete;
    FilamentBackend& operator=(const FilamentBackend&) = delete;

    // Create Filament resources for a tile's render data.
    // The backend creates and owns shared materials on first use.
    // Returns per-tile resources for later destruction.
    // The caller adds entities to the scene and applies tileTransform.
    FilamentTileResources createTile(
        filament::Engine* engine,
        const TileRenderData& data);

    // Remove entities from scene and free GPU resources.
    void destroyTile(
        filament::Engine* engine,
        filament::Scene* scene,
        FilamentTileResources& resources);

    // Free the shared Materials. Call when the Engine is being destroyed.
    void destroyMaterial(filament::Engine* engine);

private:
    filament::Material* _material = nullptr;         // unlit solid color
    filament::Material* _texturedMaterial = nullptr; // unlit with texture
    filament::Material* _pbrMaterial = nullptr;      // PBR lit (with normals)
    filament::Material* _vertexColorMaterial = nullptr; // P37: unlit with vertex colors (pnts)
    filament::Material* _transparentMaterial = nullptr; // P37: unlit transparent (alphaMode=BLEND)
};

} // namespace tilesetio
