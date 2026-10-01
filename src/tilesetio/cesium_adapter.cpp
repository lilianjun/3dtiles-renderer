// tilesetio cesium-native adapter implementation.
// Extracts RAW MEMORY DATA from CesiumGltf::Model via AccessorView.
// No GLB serialization, no gltfio.

#include "cesium_adapter.h"

#include <CesiumGltf/AccessorView.h>

#include <cstring>

namespace tilesetio {
namespace {

// Find accessor index for a named attribute.
int findAccessor(
    const CesiumGltf::MeshPrimitive& prim,
    const std::string& name) {
    auto it = prim.attributes.find(name);
    return it != prim.attributes.end() ? it->second : -1;
}

// Read vec3 float positions.
bool readPositions(
    const CesiumGltf::Model& model,
    int accessorIndex,
    std::vector<float>& out) {
    if (accessorIndex < 0)
        return false;
    CesiumGltf::AccessorView<glm::vec3> view(model, accessorIndex);
    if (view.status() != CesiumGltf::AccessorViewStatus::Valid)
        return false;
    out.resize(static_cast<size_t>(view.size()) * 3);
    for (int64_t i = 0; i < view.size(); ++i) {
        out[static_cast<size_t>(i) * 3 + 0] = view[i].x;
        out[static_cast<size_t>(i) * 3 + 1] = view[i].y;
        out[static_cast<size_t>(i) * 3 + 2] = view[i].z;
    }
    return true;
}

} // namespace

TileRenderData convertModel(
    const CesiumGltf::Model& model,
    const double tileTransform[16],
    const std::string& tileId) {
    TileRenderData out;
    out.tileId = tileId;
    std::memcpy(out.tileTransform, tileTransform, 16 * sizeof(double));

    for (const auto& mesh : model.meshes) {
        for (const auto& prim : mesh.primitives) {
            if (prim.mode != CesiumGltf::MeshPrimitive::Mode::TRIANGLES)
                continue;

            PrimitiveData pd;
            if (!readPositions(
                    model, findAccessor(prim, "POSITION"), pd.positions))
                continue;

            // Indices.
            if (prim.indices >= 0) {
                const auto& acc =
                    model.accessors[static_cast<size_t>(prim.indices)];
                if (acc.componentType ==
                    CesiumGltf::Accessor::ComponentType::UNSIGNED_SHORT) {
                    CesiumGltf::AccessorView<uint16_t> v(model, prim.indices);
                    if (v.status() == CesiumGltf::AccessorViewStatus::Valid) {
                        pd.indices.reserve(static_cast<size_t>(v.size()));
                        for (int64_t i = 0; i < v.size(); ++i)
                            pd.indices.push_back(v[i]);
                    }
                } else {
                    CesiumGltf::AccessorView<uint32_t> v(model, prim.indices);
                    if (v.status() == CesiumGltf::AccessorViewStatus::Valid) {
                        pd.indices.reserve(static_cast<size_t>(v.size()));
                        for (int64_t i = 0; i < v.size(); ++i)
                            pd.indices.push_back(v[i]);
                    }
                }
            }

            // Material color (baseColorFactor).
            if (prim.material >= 0 &&
                static_cast<size_t>(prim.material) < model.materials.size()) {
                const auto& mat =
                    model.materials[static_cast<size_t>(prim.material)];
                const auto& pbr = mat.pbrMetallicRoughness;
                if (pbr && pbr->baseColorFactor.size() == 4) {
                    pd.color[0] =
                        static_cast<float>(pbr->baseColorFactor[0]);
                    pd.color[1] =
                        static_cast<float>(pbr->baseColorFactor[1]);
                    pd.color[2] =
                        static_cast<float>(pbr->baseColorFactor[2]);
                    pd.color[3] =
                        static_cast<float>(pbr->baseColorFactor[3]);
                }
            }

            out.primitives.push_back(std::move(pd));
        }
    }

    // Compute per-primitive bbox from positions.
    for (auto& prim : out.primitives) {
        if (prim.positions.empty())
            continue;
        prim.bboxMin[0] = prim.bboxMax[0] = prim.positions[0];
        prim.bboxMin[1] = prim.bboxMax[1] = prim.positions[1];
        prim.bboxMin[2] = prim.bboxMax[2] = prim.positions[2];
        for (size_t i = 1; i < prim.positions.size() / 3; ++i) {
            float x = prim.positions[i * 3 + 0];
            float y = prim.positions[i * 3 + 1];
            float z = prim.positions[i * 3 + 2];
            if (x < prim.bboxMin[0]) prim.bboxMin[0] = x;
            if (y < prim.bboxMin[1]) prim.bboxMin[1] = y;
            if (z < prim.bboxMin[2]) prim.bboxMin[2] = z;
            if (x > prim.bboxMax[0]) prim.bboxMax[0] = x;
            if (y > prim.bboxMax[1]) prim.bboxMax[1] = y;
            if (z > prim.bboxMax[2]) prim.bboxMax[2] = z;
        }
    }

    return out;
}

} // namespace tilesetio
