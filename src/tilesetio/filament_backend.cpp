// tilesetio Filament backend implementation.
// Pure Filament API: VertexBuffer, IndexBuffer, RenderableManager.
// No gltfio, no UbershaderProvider, no GLB.

#include "filament_backend.h"

#include "pbr_color_filamat.h"
#include "unlit_color_filamat.h"
#include "unlit_textured_filamat.h"
#include "unlit_vertex_color_filamat.h"
#include "point_billboard_filamat.h"

#include <filament/IndexBuffer.h>
#include <filament/RenderableManager.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/VertexBuffer.h>

#include <math/mat3.h>
#include <math/quat.h>
#include <math/vec3.h>

#include <utils/EntityManager.h>

#include <cstring>
#include <cmath>

namespace tilesetio {
namespace {

// Convert a vec3 normal to Filament TANGENTS quaternion.
// Picks an arbitrary tangent perpendicular to the normal.
filament::math::quatf normalToTangentQuat(float nx, float ny, float nz) {
    using namespace filament::math;
    float3 n(nx, ny, nz);
    // Pick a helper vector not parallel to n
    float3 helper = std::abs(ny) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(helper, n));
    float3 b = cross(n, t);
    // Build tangent frame matrix: columns are T, B, N
    mat3f m(t, b, n);
    return mat3f::packTangentFrame(m);
}

} // namespace

FilamentTileResources FilamentBackend::createTile(
    filament::Engine* engine,
    const TileRenderData& data) {
    FilamentTileResources res;

    // Create the shared unlit material on first use.
    if (!_material) {
        _material = filament::Material::Builder()
                        .package(unlit_color_filamat, unlit_color_filamat_len)
                        .build(*engine);
    }
    // Create the shared textured material on first use.
    if (!_texturedMaterial) {
        _texturedMaterial = filament::Material::Builder()
                                .package(unlit_textured_filamat,
                                         unlit_textured_filamat_len)
                                .build(*engine);
    }
    // Create the shared PBR material on first use (for primitives with normals).
    if (!_pbrMaterial) {
        _pbrMaterial = filament::Material::Builder()
                           .package(pbr_color_filamat, pbr_color_filamat_len)
                           .build(*engine);
    }
    // P37: Create the shared vertex color material on first use (for pnts).
    if (!_vertexColorMaterial) {
        _vertexColorMaterial = filament::Material::Builder()
                                   .package(unlit_vertex_color_filamat,
                                            unlit_vertex_color_filamat_len)
                                   .build(*engine);
    }
    // P37: Billboard material for point quads (mature cross-API points).
    if (!_billboardMaterial) {
        _billboardMaterial = filament::Material::Builder()
                                 .package(point_billboard_filamat,
                                          point_billboard_filamat_len)
                                 .build(*engine);
    }

    for (const auto& prim : data.primitives) {
        if (prim.positions.empty())
            continue;

        const uint32_t vertexCount =
            static_cast<uint32_t>(prim.positions.size() / 3);
        const bool hasNormals =
            prim.normals.size() == static_cast<size_t>(vertexCount) * 3;
        const bool hasUVs =
            prim.uvs.size() == static_cast<size_t>(vertexCount) * 2;
        // P37: vertex colors for pnts point clouds.
        const bool hasColors =
            prim.colors.size() == static_cast<size_t>(vertexCount) * 3;
        // P37: Always use UNLIT (direct colors, no lighting). Per user
        // 2026-10-01, this stage uses no lighting on both sides (Cesium
        // benchmark uses pow(diffuse, 2.2) direct color). PBR would require
        // lights and would render black without them.
        const bool usePbr = false;

        // VertexBuffer: POSITION (float3), TANGENTS (float4 quaternion) if
        // we have normals, UV0 (float2) if present, COLOR (float3) if present.
        int bufferIndex = 0;
        int tangentBuffer = -1;
        int uvBuffer = -1;
        int colorBuffer = -1;
        auto vbBuilder = filament::VertexBuffer::Builder()
                             .vertexCount(vertexCount)
                             .bufferCount(1 + (hasNormals ? 1 : 0) + (hasUVs ? 1 : 0) + (hasColors ? 1 : 0))
                             .attribute(
                                 filament::VertexAttribute::POSITION,
                                 0,
                                 filament::VertexBuffer::AttributeType::FLOAT3);
        if (hasNormals) {
            tangentBuffer = ++bufferIndex;
            vbBuilder.attribute(
                filament::VertexAttribute::TANGENTS,
                static_cast<uint8_t>(tangentBuffer),
                filament::VertexBuffer::AttributeType::FLOAT4);
        }
        if (hasUVs) {
            uvBuffer = ++bufferIndex;
            vbBuilder.attribute(
                filament::VertexAttribute::UV0,
                static_cast<uint8_t>(uvBuffer),
                filament::VertexBuffer::AttributeType::FLOAT2);
        }
        // P37: vertex colors (COLOR_0) for pnts.
        if (hasColors) {
            colorBuffer = ++bufferIndex;
            vbBuilder.attribute(
                filament::VertexAttribute::COLOR,
                static_cast<uint8_t>(colorBuffer),
                filament::VertexBuffer::AttributeType::FLOAT3);
        }
        filament::VertexBuffer* vb = vbBuilder.build(*engine);

        // Copy positions (one necessary copy; Filament takes ownership via callback).
        auto* posCopy = new float[prim.positions.size()];
        std::memcpy(
            posCopy,
            prim.positions.data(),
            prim.positions.size() * sizeof(float));
        vb->setBufferAt(
            *engine,
            0,
            filament::VertexBuffer::BufferDescriptor(
                posCopy,
                prim.positions.size() * sizeof(float),
                [](void*, size_t, void* p) {
                    delete[] static_cast<float*>(p);
                },
                posCopy));

        // Convert normals to TANGENTS quaternions if present.
        if (hasNormals) {
            auto* tanCopy = new float[vertexCount * 4];
            for (uint32_t i = 0; i < vertexCount; ++i) {
                float nx = prim.normals[i * 3 + 0];
                float ny = prim.normals[i * 3 + 1];
                float nz = prim.normals[i * 3 + 2];
                auto q = normalToTangentQuat(nx, ny, nz);
                tanCopy[i * 4 + 0] = q.x;
                tanCopy[i * 4 + 1] = q.y;
                tanCopy[i * 4 + 2] = q.z;
                tanCopy[i * 4 + 3] = q.w;
            }
            vb->setBufferAt(
                *engine,
                static_cast<uint8_t>(tangentBuffer),
                filament::VertexBuffer::BufferDescriptor(
                    tanCopy,
                    vertexCount * 4 * sizeof(float),
                    [](void*, size_t, void* p) {
                        delete[] static_cast<float*>(p);
                    },
                    tanCopy));
        }

        // Copy UVs if present.
        if (hasUVs) {
            auto* uvCopy = new float[prim.uvs.size()];
            std::memcpy(uvCopy, prim.uvs.data(), prim.uvs.size() * sizeof(float));
            vb->setBufferAt(
                *engine,
                static_cast<uint8_t>(uvBuffer),
                filament::VertexBuffer::BufferDescriptor(
                    uvCopy,
                    prim.uvs.size() * sizeof(float),
                    [](void*, size_t, void* p) {
                        delete[] static_cast<float*>(p);
                    },
                    uvCopy));
        }

        // P37: Copy vertex colors if present.
        if (hasColors) {
            auto* colCopy = new float[prim.colors.size()];
            std::memcpy(colCopy, prim.colors.data(), prim.colors.size() * sizeof(float));
            vb->setBufferAt(
                *engine,
                static_cast<uint8_t>(colorBuffer),
                filament::VertexBuffer::BufferDescriptor(
                    colCopy,
                    prim.colors.size() * sizeof(float),
                    [](void*, size_t, void* p) {
                        delete[] static_cast<float*>(p);
                    },
                    colCopy));
        }
        res.vertexBuffers.push_back(vb);

        // IndexBuffer (if indexed).
        filament::IndexBuffer* ib = nullptr;
        uint32_t indexCount = 0;
        if (!prim.indices.empty()) {
            indexCount = static_cast<uint32_t>(prim.indices.size());
            ib = filament::IndexBuffer::Builder()
                     .indexCount(indexCount)
                     .bufferType(filament::IndexBuffer::IndexType::UINT)
                     .build(*engine);
            auto* idxCopy = new uint32_t[indexCount];
            std::memcpy(
                idxCopy, prim.indices.data(), indexCount * sizeof(uint32_t));
            ib->setBuffer(
                *engine,
                filament::IndexBuffer::BufferDescriptor(
                    idxCopy,
                    indexCount * sizeof(uint32_t),
                    [](void*, size_t, void* p) {
                        delete[] static_cast<uint32_t*>(p);
                    },
                    idxCopy));
            res.indexBuffers.push_back(ib);
        }

        // Material instance: textured if we have texture data, else solid color.
        filament::MaterialInstance* mi = nullptr;
        if (!prim.texPixels.empty() && prim.texWidth > 0 && prim.texHeight > 0) {
            // Create Filament Texture from RGBA pixels.
            filament::Texture* tex =
                filament::Texture::Builder()
                    .width(static_cast<uint32_t>(prim.texWidth))
                    .height(static_cast<uint32_t>(prim.texHeight))
                    .levels(1)
                    .format(filament::Texture::InternalFormat::RGBA8)
                    .sampler(filament::Texture::Sampler::SAMPLER_2D)
                    .build(*engine);
            // Copy pixels (one necessary copy).
            size_t pxSize = prim.texPixels.size();
            auto* pxCopy = new std::byte[pxSize];
            std::memcpy(pxCopy, prim.texPixels.data(), pxSize);
            filament::Texture::PixelBufferDescriptor desc(
                pxCopy,
                pxSize,
                filament::Texture::Format::RGBA,
                filament::Texture::Type::UBYTE,
                [](void*, size_t, void* p) {
                    delete[] static_cast<std::byte*>(p);
                },
                pxCopy);
            tex->setImage(*engine, 0, std::move(desc));
            res.textures.push_back(tex);

            mi = _texturedMaterial->createInstance();
            mi->setParameter(
                "color",
                filament::math::float3(
                    prim.color[0], prim.color[1], prim.color[2]));
            filament::TextureSampler sampler(
                filament::TextureSampler::MinFilter::LINEAR,
                filament::TextureSampler::MagFilter::LINEAR);
            mi->setParameter("baseColorMap", tex, sampler);
        } else if (hasColors) {
            // P37: Vertex colors (pnts point clouds) - use vertex color material.
            // The material reads COLOR attribute, no uniform color needed.
            // If billboard, use the billboard material (vertex shader expands).
            if (prim.isBillboard) {
                mi = _billboardMaterial->createInstance();
            } else {
                mi = _vertexColorMaterial->createInstance();
            }
        } else {
            // Solid color: use UNLIT (no lighting, direct color).
            // Per user 2026-10-01: lighting is not the goal for this stage.
            // Both Cesium benchmark and our renderer use direct colors.
            mi = _material->createInstance();
            mi->setParameter(
                "color",
                filament::math::float3(
                    prim.color[0], prim.color[1], prim.color[2]));
        }
        res.materialInstances.push_back(mi);

        // Renderable entity.
        utils::Entity entity = utils::EntityManager::get().create();
        filament::RenderableManager::Builder builder(1);
        builder.boundingBox({{
                                 prim.bboxMin[0],
                                 prim.bboxMin[1],
                                 prim.bboxMin[2],
                             },
                             {
                                 prim.bboxMax[0],
                                 prim.bboxMax[1],
                                 prim.bboxMax[2],
                             }})
            .material(0, mi)
            .culling(true)
            .castShadows(false)
            .receiveShadows(false);
        // P37: primitive type: 0=TRIANGLES, 1=LINES (outline), 2=POINTS (pnts).
        filament::RenderableManager::PrimitiveType filType =
            filament::RenderableManager::PrimitiveType::TRIANGLES;
        if (prim.primType == 1) {
            filType = filament::RenderableManager::PrimitiveType::LINES;
        } else if (prim.primType == 2) {
            filType = filament::RenderableManager::PrimitiveType::POINTS;
        }
        if (ib) {
            builder.geometry(
                0,
                filType,
                vb,
                ib,
                0,
                indexCount);
        } else {
            builder.geometry(
                0,
                filType,
                vb,
                0,
                vertexCount);
        }
        auto result = builder.build(*engine, entity);
        if (result != filament::RenderableManager::Builder::Result::Success) {
            engine->destroy(entity);
            continue;
        }

        res.entities.push_back(entity);
    }

    return res;
}

void FilamentBackend::destroyTile(
    filament::Engine* engine,
    filament::Scene* scene,
    FilamentTileResources& resources) {
    if (scene && !resources.entities.empty()) {
        scene->removeEntities(
            resources.entities.data(), resources.entities.size());
    }
    for (auto e : resources.entities)
        engine->destroy(e);
    for (auto vb : resources.vertexBuffers)
        engine->destroy(vb);
    for (auto ib : resources.indexBuffers)
        engine->destroy(ib);
    for (auto mi : resources.materialInstances)
        engine->destroy(mi);
    for (auto tex : resources.textures)
        engine->destroy(tex);
    resources = FilamentTileResources{};
}

void FilamentBackend::destroyMaterial(filament::Engine* engine) {
    if (_material) {
        engine->destroy(_material);
        _material = nullptr;
    }
    if (_texturedMaterial) {
        engine->destroy(_texturedMaterial);
        _texturedMaterial = nullptr;
    }
    if (_pbrMaterial) {
        engine->destroy(_pbrMaterial);
        _pbrMaterial = nullptr;
    }
}

} // namespace tilesetio
