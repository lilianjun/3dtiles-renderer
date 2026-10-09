// bgfx_backend.cpp — bgfx rendering backend implementation.
// Replaces filament_backend.cpp (2026-10-09: Filament -> bgfx migration).

#include "bgfx_backend.h"
#include <cstring>

namespace tilesetio {

// Helper: load a compiled shader binary from file
static bgfx::ShaderHandle loadShader(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return BGFX_INVALID_HANDLE;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    const bgfx::Memory* mem = bgfx::alloc(uint32_t(size));
    fread(mem->data, 1, size, f);
    fclose(f);
    return bgfx::createShader(mem);
}

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

    // Load compiled shaders (via shaderc output)
    // Shaders are compiled at build time to build/shaders/
    // For now, try loading from standard locations
    const char* shaderPaths[] = {
        "./shaders/", "./build/shaders/", "/tmp/shaders/",
    };
    bgfx::ShaderHandle vsColor = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle fsColor = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle vsTex = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle fsTex = BGFX_INVALID_HANDLE;
    
    for (const char* base : shaderPaths) {
        char path[512];
        snprintf(path, sizeof(path), "%sunlit_color_vs.bin", base);
        vsColor = loadShader(path);
        if (bgfx::isValid(vsColor)) break;
    }
    // Note: Full shader loading to be completed when shaderc is integrated
    // For now, programs remain invalid (rendering will be skipped)

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
        size_t vertexCount = prim.positions.size() / 3;
        if (vertexCount == 0) continue;

        // Create vertex layout
        bgfx::VertexLayout layout;
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
            .end();

        // Interleave vertex data: pos(3) + normal(3) + uv(2) + color(4) = 12 floats
        const uint32_t stride = 12 * sizeof(float);
        const uint32_t numBytes = static_cast<uint32_t>(vertexCount * stride);
        const bgfx::Memory* vmem = bgfx::alloc(numBytes);
        float* vdata = reinterpret_cast<float*>(vmem->data);

        bool hasNormals = prim.normals.size() >= vertexCount * 3;
        bool hasUvs = prim.uvs.size() >= vertexCount * 2;
        bool hasColors = prim.colors.size() >= vertexCount * 4;

        for (size_t i = 0; i < vertexCount; ++i) {
            float* v = vdata + i * 12;
            // Position (required)
            v[0] = prim.positions[i * 3 + 0];
            v[1] = prim.positions[i * 3 + 1];
            v[2] = prim.positions[i * 3 + 2];
            // Normal (optional, default up)
            if (hasNormals) {
                v[3] = prim.normals[i * 3 + 0];
                v[4] = prim.normals[i * 3 + 1];
                v[5] = prim.normals[i * 3 + 2];
            } else {
                v[3] = 0.0f; v[4] = 1.0f; v[5] = 0.0f;
            }
            // UV (optional, default 0)
            if (hasUvs) {
                v[6] = prim.uvs[i * 2 + 0];
                v[7] = prim.uvs[i * 2 + 1];
            } else {
                v[6] = 0.0f; v[7] = 0.0f;
            }
            // Color (optional, default prim.color or white)
            if (hasColors) {
                v[8] = prim.colors[i * 4 + 0];
                v[9] = prim.colors[i * 4 + 1];
                v[10] = prim.colors[i * 4 + 2];
                v[11] = prim.colors[i * 4 + 3];
            } else {
                v[8] = prim.color[0];
                v[9] = prim.color[1];
                v[10] = prim.color[2];
                v[11] = prim.color[3];
            }
        }

        bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vmem, layout);
        resources.vertexBuffers.push_back(vbh);

        // Create index buffer
        if (!prim.indices.empty()) {
            const bgfx::Memory* mem = bgfx::copy(
                prim.indices.data(),
                static_cast<uint32_t>(prim.indices.size() * sizeof(uint32_t)));
            bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(mem, BGFX_BUFFER_INDEX32);
            resources.indexBuffers.push_back(ibh);
            resources.indexCounts.push_back(static_cast<uint32_t>(prim.indices.size()));
        } else {
            // Non-indexed: index count = vertex count
            resources.indexBuffers.push_back(BGFX_INVALID_HANDLE);
            resources.indexCounts.push_back(static_cast<uint32_t>(vertexCount));
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
                BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE,
                mem);
            resources.textures.push_back(th);
        } else {
            resources.textures.push_back(BGFX_INVALID_HANDLE);
        }

        // Select program based on whether texture is present
        if (bgfx::isValid(resources.textures.back())) {
            resources.programs.push_back(_unlitTexturedProgram);
        } else {
            resources.programs.push_back(_unlitColorProgram);
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
    size_t primCount = resources.vertexBuffers.size();
    for (size_t i = 0; i < primCount; ++i) {
        bgfx::VertexBufferHandle vbh = resources.vertexBuffers[i];
        if (!bgfx::isValid(vbh)) continue;

        bgfx::IndexBufferHandle ibh = BGFX_INVALID_HANDLE;
        if (i < resources.indexBuffers.size()) {
            ibh = resources.indexBuffers[i];
        }

        bgfx::ProgramHandle prog = BGFX_INVALID_HANDLE;
        if (i < resources.programs.size()) {
            prog = resources.programs[i];
        }
        if (!bgfx::isValid(prog)) continue;

        // Set model-view-proj matrix
        bgfx::setUniform(_u_modelViewProj, modelMatrix);

        // Set vertex and index buffers
        bgfx::setVertexBuffer(0, vbh);
        if (bgfx::isValid(ibh)) {
            bgfx::setIndexBuffer(ibh);
        }

        // Set texture if present
        if (i < resources.textures.size() && bgfx::isValid(resources.textures[i])) {
            bgfx::setTexture(0, _s_texColor, resources.textures[i]);
        }

        // Submit
        bgfx::submit(viewId, prog);
    }
}

} // namespace tilesetio
