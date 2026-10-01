// tilesetio cesium-native adapter implementation.
// Reads CesiumGltf::Model directly via AccessorView — no GLB serialization.

#include "cesium_adapter.h"

#include <iostream>

#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/ExtensionCesiumRTC.h>
#include <CesiumUtility/Math.h>

#include <cstring>

namespace tilesetio {
namespace {

// Copy a column-major 4x4 double matrix.
void copyMat4(const double src[16], double dst[16]) {
    std::memcpy(dst, src, 16 * sizeof(double));
}

// Compose node local transform from TRS or matrix.
void composeNodeTransform(const CesiumGltf::Node& node, float out[16]) {
    // Default: identity.
    for (int i = 0; i < 16; ++i)
        out[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    if (!node.matrix.empty()) {
        for (int i = 0; i < 16 && i < static_cast<int>(node.matrix.size()); ++i)
            out[i] = static_cast<float>(node.matrix[i]);
        return;
    }

    // TRS compose: T * R * S, column-major.
    float t[3] = {0, 0, 0};
    float s[3] = {1, 1, 1};
    float q[4] = {0, 0, 0, 1}; // x,y,z,w
    if (node.translation.size() == 3) {
        t[0] = static_cast<float>(node.translation[0]);
        t[1] = static_cast<float>(node.translation[1]);
        t[2] = static_cast<float>(node.translation[2]);
    }
    if (node.scale.size() == 3) {
        s[0] = static_cast<float>(node.scale[0]);
        s[1] = static_cast<float>(node.scale[1]);
        s[2] = static_cast<float>(node.scale[2]);
    }
    if (node.rotation.size() == 4) {
        q[0] = static_cast<float>(node.rotation[0]);
        q[1] = static_cast<float>(node.rotation[1]);
        q[2] = static_cast<float>(node.rotation[2]);
        q[3] = static_cast<float>(node.rotation[3]);
    }

    // Quaternion to 3x3 (column-major).
    const float xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
    const float xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2];
    const float wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];
    float r[9] = {
        1 - 2 * (yy + zz), 2 * (xy + wz),     2 * (xz - wy),
        2 * (xy - wz),     1 - 2 * (xx + zz), 2 * (yz + wx),
        2 * (xz + wy),     2 * (yz - wx),     1 - 2 * (xx + yy),
    };

    // M = T * R * S
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            out[col * 4 + row] = r[col * 3 + row] * s[col];
        }
    }
    out[12] = t[0];
    out[13] = t[1];
    out[14] = t[2];
    out[15] = 1.0f;
}

// Read a vec3 float attribute (POSITION, NORMAL) into out (3*N floats).
template <typename T>
bool readVec3(
    const CesiumGltf::Model& model,
    int accessorIndex,
    std::vector<float>& out) {
    if (accessorIndex < 0)
        return false;
    CesiumGltf::AccessorView<T> view(model, accessorIndex);
    if (view.status() != CesiumGltf::AccessorViewStatus::Valid)
        return false;
    out.resize(static_cast<size_t>(view.size()) * 3);
    for (int64_t i = 0; i < view.size(); ++i) {
        const T& v = view[i];
        out[static_cast<size_t>(i) * 3 + 0] = static_cast<float>(v.x);
        out[static_cast<size_t>(i) * 3 + 1] = static_cast<float>(v.y);
        out[static_cast<size_t>(i) * 3 + 2] = static_cast<float>(v.z);
    }
    return true;
}

// Read a vec2 float attribute (TEXCOORD_0) into out (2*N floats).
template <typename T>
bool readVec2(
    const CesiumGltf::Model& model,
    int accessorIndex,
    std::vector<float>& out) {
    if (accessorIndex < 0)
        return false;
    CesiumGltf::AccessorView<T> view(model, accessorIndex);
    if (view.status() != CesiumGltf::AccessorViewStatus::Valid)
        return false;
    out.resize(static_cast<size_t>(view.size()) * 2);
    for (int64_t i = 0; i < view.size(); ++i) {
        const T& v = view[i];
        out[static_cast<size_t>(i) * 2 + 0] = static_cast<float>(v.x);
        out[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(v.y);
    }
    return true;
}

int findAccessor(
    const CesiumGltf::MeshPrimitive& prim,
    const std::string& name) {
    auto it = prim.attributes.find(name);
    if (it == prim.attributes.end())
        return -1;
    return it->second;
}

MaterialParams convertMaterial(const CesiumGltf::Material& m) {
    MaterialParams out;
    out.name = m.name;

    const auto& pbr = m.pbrMetallicRoughness;
    if (pbr) {
        const auto& bc = pbr->baseColorFactor;
        if (bc.size() == 4) {
            out.baseColorFactor[0] = static_cast<float>(bc[0]);
            out.baseColorFactor[1] = static_cast<float>(bc[1]);
            out.baseColorFactor[2] = static_cast<float>(bc[2]);
            out.baseColorFactor[3] = static_cast<float>(bc[3]);
        }
        out.metallicFactor = static_cast<float>(pbr->metallicFactor);
        out.roughnessFactor = static_cast<float>(pbr->roughnessFactor);
        // TODO(T3): texture indices — resolve via model.textures.
    }

    const auto& emissive = m.emissiveFactor;
    if (emissive.size() == 3) {
        out.emissiveFactor[0] = static_cast<float>(emissive[0]);
        out.emissiveFactor[1] = static_cast<float>(emissive[1]);
        out.emissiveFactor[2] = static_cast<float>(emissive[2]);
    }

    out.doubleSided = m.doubleSided;

    const std::string& am = m.alphaMode;
    if (am == "MASK")
        out.alphaMode = MaterialParams::AlphaMode::Mask;
    else if (am == "BLEND")
        out.alphaMode = MaterialParams::AlphaMode::Blend;
    else
        out.alphaMode = MaterialParams::AlphaMode::Opaque;
    out.alphaCutoff = static_cast<float>(m.alphaCutoff);

    return out;
}

} // namespace

TileRenderData convertModel(
    const CesiumGltf::Model& model,
    const double tileTransform[16],
    const std::string& tileId) {
    TileRenderData out;
    out.tileId = tileId;
    // tileTransform already includes RTC_CENTER (extracted from CESIUM_RTC
    // extension in prepareInLoadThread) and localOrigin subtraction, in the
    // correct order: modelMatrix * tile * RTC - localOrigin.
    // See tileset.cpp prepareInMainThread.
    copyMat4(tileTransform, out.tileTransform);

    std::cerr << "[tilesetio] convertModel: tile=" << tileId
              << " meshes=" << model.meshes.size()
              << " materials=" << model.materials.size()
              << " nodes=" << model.nodes.size() << std::endl;

    // Materials.
    out.materials.reserve(model.materials.size());
    for (const auto& m : model.materials)
        out.materials.push_back(convertMaterial(m));
    if (out.materials.empty())
        out.materials.emplace_back(); // default material

    // Nodes: flatten hierarchy. NodeData.parent refers to index in out.nodes.
    out.nodes.reserve(model.nodes.size());
    for (const auto& node : model.nodes) {
        NodeData nd;
        composeNodeTransform(node, nd.transform);
        nd.parent = -1; // resolved below
        out.nodes.push_back(nd);
        std::cerr << "[tilesetio] node transform: ["
                  << nd.transform[12] << "," << nd.transform[13] << ","
                  << nd.transform[14] << "]" << std::endl;
    }
    // Resolve parents: find which node lists this one as a child.
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        for (int child : model.nodes[i].children) {
            if (child >= 0 &&
                static_cast<size_t>(child) < out.nodes.size()) {
                out.nodes[static_cast<size_t>(child)].parent =
                    static_cast<int>(i);
            }
        }
    }

    // Meshes -> primitives. Each primitive references its node via the
    // mesh->node mapping: find nodes that reference this mesh.
    // Build mesh->node list first.
    std::vector<int> meshToNode(model.meshes.size(), -1);
    for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
        int meshIdx = model.nodes[ni].mesh;
        if (meshIdx >= 0 &&
            static_cast<size_t>(meshIdx) < meshToNode.size() &&
            meshToNode[static_cast<size_t>(meshIdx)] < 0) {
            meshToNode[static_cast<size_t>(meshIdx)] = static_cast<int>(ni);
        }
    }

    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const auto& mesh = model.meshes[mi];
        const int nodeIdx =
            static_cast<int>(mi) < static_cast<int>(meshToNode.size())
                ? meshToNode[mi]
                : -1;
        for (const auto& prim : mesh.primitives) {
            // Only triangles for now.
            if (prim.mode != CesiumGltf::MeshPrimitive::Mode::TRIANGLES &&
                prim.mode != CesiumGltf::MeshPrimitive::Mode::TRIANGLES) {
                continue;
            }

            PrimitiveData pd;
            pd.nodeIndex =
                nodeIdx >= 0 ? static_cast<uint32_t>(nodeIdx) : 0;

            // POSITION (required).
            const int posAcc = findAccessor(prim, "POSITION");
            if (posAcc >= 0) {
                const auto& acc = model.accessors[static_cast<size_t>(posAcc)];
                std::cerr << "[tilesetio] prim POSITION acc=" << posAcc
                          << " count=" << acc.count << std::endl;
            }
            if (!readVec3<glm::vec3>(model, posAcc, pd.positions))
                continue; // skip primitives without valid positions

            // NORMAL (optional).
            const int nrmAcc = findAccessor(prim, "NORMAL");
            readVec3<glm::vec3>(model, nrmAcc, pd.normals);

            // TEXCOORD_0 (optional).
            const int uvAcc = findAccessor(prim, "TEXCOORD_0");
            readVec2<glm::vec2>(model, uvAcc, pd.texcoords);

            // Indices (optional).
            if (prim.indices >= 0) {
                const CesiumGltf::Accessor& acc =
                    model.accessors[static_cast<size_t>(prim.indices)];
                // Handle UINT16 and UINT32 index types.
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

            // Material.
            pd.materialIndex =
                prim.material >= 0 &&
                        static_cast<size_t>(prim.material) <
                            out.materials.size()
                    ? static_cast<uint32_t>(prim.material)
                    : 0;

            out.primitives.push_back(std::move(pd));
        }
    }

    // TODO(T3): textures — decode images to RGBA.

    return out;
}

} // namespace tilesetio
