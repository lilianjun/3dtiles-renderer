// tilesetio Filament backend implementation.
// Pure Filament API: VertexBuffer, IndexBuffer, RenderableManager.
// No gltfio, no UbershaderProvider, no GLB.

#include "filament_backend.h"

#include "unlit_color_filamat.h"
#include "unlit_textured_filamat.h"

#include <filament/IndexBuffer.h>
#include <filament/RenderableManager.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/VertexBuffer.h>

#include <utils/EntityManager.h>

#include <cstring>

namespace tilesetio {

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

    for (const auto& prim : data.primitives) {
        if (prim.positions.empty())
            continue;

        const uint32_t vertexCount =
            static_cast<uint32_t>(prim.positions.size() / 3);
        const bool hasUVs =
            prim.uvs.size() == static_cast<size_t>(vertexCount) * 2;

        // VertexBuffer: POSITION (float3), plus UV0 (float2) if present.
        auto vbBuilder = filament::VertexBuffer::Builder()
                             .vertexCount(vertexCount)
                             .bufferCount(hasUVs ? 2 : 1)
                             .attribute(
                                 filament::VertexAttribute::POSITION,
                                 0,
                                 filament::VertexBuffer::AttributeType::FLOAT3);
        if (hasUVs) {
            vbBuilder.attribute(
                filament::VertexAttribute::UV0,
                1,
                filament::VertexBuffer::AttributeType::FLOAT2);
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

        // Copy UVs if present.
        if (hasUVs) {
            auto* uvCopy = new float[prim.uvs.size()];
            std::memcpy(uvCopy, prim.uvs.data(), prim.uvs.size() * sizeof(float));
            vb->setBufferAt(
                *engine,
                1,
                filament::VertexBuffer::BufferDescriptor(
                    uvCopy,
                    prim.uvs.size() * sizeof(float),
                    [](void*, size_t, void* p) {
                        delete[] static_cast<float*>(p);
                    },
                    uvCopy));
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
        } else {
            // Solid color.
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
        if (ib) {
            builder.geometry(
                0,
                filament::RenderableManager::PrimitiveType::TRIANGLES,
                vb,
                ib,
                0,
                indexCount);
        } else {
            builder.geometry(
                0,
                filament::RenderableManager::PrimitiveType::TRIANGLES,
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
}

} // namespace tilesetio
