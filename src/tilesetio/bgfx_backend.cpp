// bgfx_backend.cpp — bgfx rendering backend implementation.
// Replaces filament_backend.cpp (2026-10-09: Filament -> bgfx migration).

#include "bgfx_backend.h"
#include <cstring>

namespace tilesetio {

BgfxBackend::~BgfxBackend() {
    shutdown();
}

bool BgfxBackend::init() {
    if (_initialized) {
        return true;
    }

    // Create uniforms
    _u_modelViewProj = bgfx::createUniform("u_modelViewProj", bgfx::UniformType::Mat4);
    _s_texColor = bgfx::createUniform("s_texColor", bgfx::UniformType::Sampler);

    // TODO: Load compiled shaders (via shaderc)
    // For now, shaders will be loaded from files

    _initialized = true;
    return true;
}

void BgfxBackend::shutdown() {
    if (!_initialized) {
        return;
    }

    if (bgfx::isValid(_unlitColorProgram)) {
        bgfx::destroy(_unlitColorProgram);
    }
    if (bgfx::isValid(_unlitTexturedProgram)) {
        bgfx::destroy(_unlitTexturedProgram);
    }
    if (bgfx::isValid(_u_modelViewProj)) {
        bgfx::destroy(_u_modelViewProj);
    }
    if (bgfx::isValid(_s_texColor)) {
        bgfx::destroy(_s_texColor);
    }

    _initialized = false;
}

BgfxTileResources BgfxBackend::createTile(const TileRenderData& data) {
    BgfxTileResources resources;

    for (const auto& prim : data.primitives) {
        // Create vertex layout
        bgfx::VertexLayout layout;
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
            .end();

        // Create vertex buffer
        // TODO: Interleave vertex data from prim
        // For now, placeholder

        // Create index buffer
        if (!prim.indices.empty()) {
            const bgfx::Memory* mem = bgfx::copy(
                prim.indices.data(),
                static_cast<uint32_t>(prim.indices.size() * sizeof(uint32_t)));
            bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(mem, BGFX_BUFFER_INDEX32);
            resources.indexBuffers.push_back(ibh);
            resources.indexCounts.push_back(static_cast<uint32_t>(prim.indices.size()));
        }

        // Create texture if present
        if (!prim.texPixels.empty() && prim.texWidth > 0 && prim.texHeight > 0) {
            const bgfx::Memory* mem = bgfx::copy(
                prim.texPixels.data(),
                static_cast<uint32_t>(prim.texPixels.size()));
            bgfx::TextureHandle th = bgfx::createTexture2D(
                static_cast<uint16_t>(prim.texWidth),
                static_cast<uint16_t>(prim.texHeight),
                false, 1,
                bgfx::TextureFormat::RGBA8,
                BGFX_TEXTURE_NONE,
                mem);
            resources.textures.push_back(th);
        }
    }

    return resources;
}

void BgfxBackend::destroyTile(BgfxTileResources& resources) {
    for (auto vbh : resources.vertexBuffers) {
        if (bgfx::isValid(vbh)) {
            bgfx::destroy(vbh);
        }
    }
    for (auto ibh : resources.indexBuffers) {
        if (bgfx::isValid(ibh)) {
            bgfx::destroy(ibh);
        }
    }
    for (auto th : resources.textures) {
        if (bgfx::isValid(th)) {
            bgfx::destroy(th);
        }
    }
    resources.vertexBuffers.clear();
    resources.indexBuffers.clear();
    resources.textures.clear();
    resources.indexCounts.clear();
}

void BgfxBackend::submitTile(
    uint8_t viewId,
    const BgfxTileResources& resources,
    const float* modelMatrix) {
    // TODO: Submit draw calls
    // For each primitive:
    //   bgfx::setVertexBuffer(0, vbh);
    //   bgfx::setIndexBuffer(ibh);
    //   bgfx::setUniform(_u_modelViewProj, modelMatrix);
    //   bgfx::submit(viewId, program);
}

} // namespace tilesetio
