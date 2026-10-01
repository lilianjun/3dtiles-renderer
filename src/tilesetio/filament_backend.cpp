// tilesetio Filament backend implementation.

#include "filament_backend.h"

#include <iostream>

#include <filament/IndexBuffer.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>

#include <utils/EntityManager.h>

namespace tilesetio {
namespace {

// Build a gltfio MaterialKey from our neutral MaterialParams.
filament::gltfio::MaterialKey makeKey(const MaterialParams& mp) {
    filament::gltfio::MaterialKey key{};
    // Zero-init (POD with bitfields).
    std::memset(&key, 0, sizeof(key));
    // T1 debug: force doubleSided to rule out winding order issues.
    key.doubleSided = true;
    // T1: unlit=true (no lights in test scene; lit would be black).
    key.unlit = true;
    key.hasVertexColors = false;
    key.hasBaseColorTexture = mp.baseColorTexture >= 0;
    key.hasNormalTexture = false; // TODO(T3)
    key.hasOcclusionTexture = false;
    key.hasEmissiveTexture = false; // TODO(T3)
    key.useSpecularGlossiness = false;
    switch (mp.alphaMode) {
    case MaterialParams::AlphaMode::Opaque:
        key.alphaMode = filament::gltfio::AlphaMode::OPAQUE;
        break;
    case MaterialParams::AlphaMode::Mask:
        key.alphaMode = filament::gltfio::AlphaMode::MASK;
        break;
    case MaterialParams::AlphaMode::Blend:
        key.alphaMode = filament::gltfio::AlphaMode::BLEND;
        break;
    }
    key.hasMetallicRoughnessTexture = mp.metallicRoughnessTexture >= 0;
    return key;
}

void setMaterialParams(
    filament::MaterialInstance* mi,
    const MaterialParams& mp) {
    // Standard Filament glTF ubershader parameter names.
    mi->setParameter(
        "baseColorFactor",
        filament::math::float4(
            mp.baseColorFactor[0],
            mp.baseColorFactor[1],
            mp.baseColorFactor[2],
            mp.baseColorFactor[3]));
    mi->setParameter("metallicFactor", mp.metallicFactor);
    mi->setParameter("roughnessFactor", mp.roughnessFactor);
    mi->setParameter(
        "emissiveFactor",
        filament::math::float3(
            mp.emissiveFactor[0],
            mp.emissiveFactor[1],
            mp.emissiveFactor[2]));
    if (mp.alphaMode == MaterialParams::AlphaMode::Mask) {
        mi->setParameter("alphaCutoff", mp.alphaCutoff);
    }
}

// Compute world transform for a node: tileTransform * nodeChain.
// nodeChain is built by walking parents from the primitive's node to root.
filament::math::mat4f computeNodeWorld(
    const TileRenderData& data,
    uint32_t nodeIndex) {
    // Start with tile transform (double -> float).
    filament::math::mat4f world;
    for (int i = 0; i < 16; ++i)
        world[i / 4][i % 4] =
            static_cast<float>(data.tileTransform[i]);

    // Collect node chain from root to leaf.
    std::vector<uint32_t> chain;
    uint32_t idx = nodeIndex;
    while (idx < data.nodes.size()) {
        chain.push_back(idx);
        int parent = data.nodes[idx].parent;
        if (parent < 0)
            break;
        idx = static_cast<uint32_t>(parent);
    }
    // Apply from root down to leaf: world = world * nodeRoot * ... * nodeLeaf
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto& nd = data.nodes[*it];
        filament::math::mat4f local;
        for (int i = 0; i < 16; ++i)
            local[i / 4][i % 4] = nd.transform[i];
        world = world * local;
    }
    return world;
}

} // namespace

FilamentTileResources FilamentBackend::createTile(
    filament::Engine* engine,
    const TileRenderData& data,
    filament::gltfio::MaterialProvider* materialProvider) {
    FilamentTileResources res;

    // Create material instances (one per unique material).
    std::vector<filament::MaterialInstance*> materialInstances;
    materialInstances.reserve(data.materials.size());
    for (const auto& mp : data.materials) {
        filament::gltfio::MaterialKey key = makeKey(mp);
        filament::gltfio::UvMap uvmap{};
        uvmap.fill(filament::gltfio::UvSet::UV0);
        filament::MaterialInstance* mi =
            materialProvider->createMaterialInstance(
                &key, &uvmap, mp.name.c_str());
        std::cerr << "[tilesetio] material '" << mp.name << "': mi="
                  << (mi ? "OK" : "NULL")
                  << " baseColor=[" << mp.baseColorFactor[0] << ","
                  << mp.baseColorFactor[1] << "," << mp.baseColorFactor[2]
                  << "]" << std::endl;
        // T1 debug: re-enable setMaterialParams.
        if (mi)
            setMaterialParams(mi, mp);
        materialInstances.push_back(mi);
    }
    // Track for destruction.
    res.materialInstances = materialInstances;

    // Create renderables (one per primitive).
    std::cerr << "[tilesetio] createTile: primitives=" << data.primitives.size()
              << " materials=" << data.materials.size() << std::endl;
    std::cerr << "[tilesetio] tileTransform: ["
              << data.tileTransform[12] << "," << data.tileTransform[13]
              << "," << data.tileTransform[14] << "]" << std::endl;
    for (const auto& prim : data.primitives) {
        if (prim.positions.empty())
            continue;
        // Debug: dump first vertex and indices.
        std::cerr << "[tilesetio] prim verts=" << prim.positions.size()/3
                  << " indices=" << prim.indices.size()
                  << " first_v=[" << prim.positions[0] << ","
                  << prim.positions[1] << "," << prim.positions[2] << "]"
                  << " first_i=[" << (prim.indices.empty() ? -1 : (int)prim.indices[0])
                  << "," << (prim.indices.size() > 1 ? (int)prim.indices[1] : -1)
                  << "," << (prim.indices.size() > 2 ? (int)prim.indices[2] : -1)
                  << "]" << std::endl;

        const uint32_t vertexCount =
            static_cast<uint32_t>(prim.positions.size() / 3);
        std::cerr << "[tilesetio] primitive: verts=" << vertexCount
                  << " indices=" << prim.indices.size()
                  << " mat=" << prim.materialIndex
                  << " node=" << prim.nodeIndex << std::endl;

        // VertexBuffer: POSITION + TANGENTS + COLOR(dummy) + UV0/UV1(dummy).
        // The ubershader variant requires 0x1f (POSITION,TANGENTS,COLOR,UV0,UV1).
        // T1: dummy values; TODO(T2): real data.
        auto vbBuilder = filament::VertexBuffer::Builder()
                .vertexCount(vertexCount)
                .bufferCount(1)
                .attribute(
                    filament::VertexAttribute::POSITION,
                    0,
                    filament::VertexBuffer::AttributeType::FLOAT3)
                .attribute(
                    filament::VertexAttribute::TANGENTS,
                    0,
                    filament::VertexBuffer::AttributeType::FLOAT4,
                    12)
                .attribute(
                    filament::VertexAttribute::COLOR,
                    0,
                    filament::VertexBuffer::AttributeType::FLOAT4,
                    28)
                .attribute(
                    filament::VertexAttribute::UV0,
                    0,
                    filament::VertexBuffer::AttributeType::FLOAT2,
                    44)
                .attribute(
                    filament::VertexAttribute::UV1,
                    0,
                    filament::VertexBuffer::AttributeType::FLOAT2,
                    52);
        filament::VertexBuffer* vb = vbBuilder.build(*engine);

        // Interleave: position(12) + tangents(16) + color(16) + uv0(8) + uv1(8) = 60.
        const size_t stride = 60;
        auto* interleaved = new uint8_t[vertexCount * stride];
        for (uint32_t v = 0; v < vertexCount; ++v) {
            uint8_t* dst = interleaved + v * stride;
            std::memcpy(dst, &prim.positions[v * 3], 12);
            // Identity quaternion (0,0,0,1) as dummy tangent frame.
            static const float identityQuat[4] = {0, 0, 0, 1};
            std::memcpy(dst + 12, identityQuat, 16);
            // White color (1,1,1,1).
            static const float white[4] = {1, 1, 1, 1};
            std::memcpy(dst + 28, white, 16);
            // Zero UVs.
            static const float zeroUV[2] = {0, 0};
            std::memcpy(dst + 44, zeroUV, 8);
            std::memcpy(dst + 52, zeroUV, 8);
        }
        vb->setBufferAt(
            *engine,
            0,
            filament::VertexBuffer::BufferDescriptor(
                interleaved,
                vertexCount * stride,
                [](void*, size_t, void* p) {
                    delete[] static_cast<uint8_t*>(p);
                },
                interleaved));
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

        // Material.
        filament::MaterialInstance* mi =
            prim.materialIndex < materialInstances.size()
                ? materialInstances[prim.materialIndex]
                : nullptr;
        if (!mi)
            continue;

        // Entity + Renderable.
        utils::Entity entity =
            utils::EntityManager::get().create();
        filament::RenderableManager::Builder builder(1);
        builder.boundingBox({{ -1000, -1000, -1000 }, { 1000, 1000, 1000 }});
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
        builder.material(0, mi);
        // T1 debug: disable culling to isolate transform/material issues.
        builder.culling(false);
        builder.castShadows(false);
        builder.receiveShadows(false);
        auto result = builder.build(*engine, entity);
        if (result !=
            filament::RenderableManager::Builder::Result::Success) {
            std::cerr << "[tilesetio] builder.build FAILED for primitive"
                      << std::endl;
            utils::EntityManager::get().destroy(entity);
            continue;
        }
        std::cerr << "[tilesetio] built entity " << (int)entity.getId()
                  << " for primitive" << std::endl;

        // Transform: tileTransform * node chain.
        filament::math::mat4f world =
            computeNodeWorld(data, prim.nodeIndex);
        auto& tcm = engine->getTransformManager();
        tcm.create(entity);
        tcm.setTransform(tcm.getInstance(entity), world);

        // Note: do NOT add to scene here. Visibility is controlled by
        // updateTileVisibility() per Cesium LOD. createTile only creates
        // resources.
        res.entities.push_back(entity);
    }

    return res;
}

void FilamentBackend::destroyTile(
    filament::Engine* engine,
    filament::Scene* scene,
    FilamentTileResources& resources) {
    for (auto e : resources.entities) {
        scene->removeEntities(&e, 1);
        engine->destroy(e);
    }
    for (auto* vb : resources.vertexBuffers)
        engine->destroy(vb);
    for (auto* ib : resources.indexBuffers)
        engine->destroy(ib);
    for (auto* mi : resources.materialInstances)
        engine->destroy(mi);
    resources.entities.clear();
    resources.vertexBuffers.clear();
    resources.indexBuffers.clear();
    resources.materialInstances.clear();
}

} // namespace tilesetio
