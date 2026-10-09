#pragma once

// tilesetio bgfx backend: converts tilesetio::TileRenderData (raw memory
// data) to bgfx resources.
// Replaces filament_backend.h (2026-10-09: Filament -> bgfx migration).

#include "core.h"

#include <bgfx/bgfx.h>
#include <vector>

namespace tilesetio {

// bgfx-side resources for one tile.
struct BgfxTileResources {
    std::vector<bgfx::VertexBufferHandle> vertexBuffers;
    std::vector<bgfx::IndexBufferHandle> indexBuffers;
    std::vector<bgfx::TextureHandle> textures;
    std::vector<bgfx::UniformHandle> uniforms;
    // Per-primitive program handles (shared, not owned per-tile)
    std::vector<bgfx::ProgramHandle> programs;
    // Number of indices per primitive (for draw calls)
    std::vector<uint32_t> indexCounts;
};

class BgfxBackend {
public:
    BgfxBackend() = default;
    ~BgfxBackend();

    // Non-copyable.
    BgfxBackend(const BgfxBackend&) = delete;
    BgfxBackend& operator=(const BgfxBackend&) = delete;

    // Initialize shared resources (shaders, etc.). Call once.
    bool init();

    // Shutdown and free shared resources.
    void shutdown();

    // Create bgfx resources for a tile's render data.
    // Returns per-tile resources for later destruction.
    BgfxTileResources createTile(const TileRenderData& data);

    // Free GPU resources for a tile.
    void destroyTile(BgfxTileResources& resources);

    // Submit draw calls for a tile. Called each frame.
    // viewId: bgfx view id, program: shader program to use
    void submitTile(
        uint8_t viewId,
        const BgfxTileResources& resources,
        const float* modelMatrix);

private:
    bool _initialized = false;
    // Shared shader programs
    bgfx::ProgramHandle _unlitColorProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle _unlitTexturedProgram = BGFX_INVALID_HANDLE;
    // Uniforms
    bgfx::UniformHandle _u_modelViewProj = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle _s_texColor = BGFX_INVALID_HANDLE;
};

} // namespace tilesetio
