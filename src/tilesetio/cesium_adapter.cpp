// tilesetio cesium-native adapter implementation.
// Extracts RAW MEMORY DATA from CesiumGltf::Model via AccessorView.
// No GLB serialization, no gltfio.

#include "cesium_adapter.h"

#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/ExtensionCesiumPrimitiveOutline.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <functional>

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

// Read vec2 float UVs.
bool readUVs(
    const CesiumGltf::Model& model,
    int accessorIndex,
    std::vector<float>& out) {
    if (accessorIndex < 0)
        return false;
    CesiumGltf::AccessorView<glm::vec2> view(model, accessorIndex);
    if (view.status() != CesiumGltf::AccessorViewStatus::Valid)
        return false;
    out.resize(static_cast<size_t>(view.size()) * 2);
    for (int64_t i = 0; i < view.size(); ++i) {
        out[static_cast<size_t>(i) * 2 + 0] = view[i].x;
        out[static_cast<size_t>(i) * 2 + 1] = view[i].y;
    }
    return true;
}

// Read vec3 float normals.
bool readNormals(
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
    // P37: Check if model has large ECEF translation in node hierarchy
    // (e.g., RTC_CENTER in bare glTF). If so, we'll keep vertices local
    // and fold the translation into out.tileTransform.
    bool hasLargeTrans = false;
    glm::dvec3 largeTrans(0.0);

    // P37: For non-instanced models, convert glTF Y-up to Z-up (ECEF) after
    // node transforms. glTF node hierarchies may contain RTC_CENTER nodes
    // with ECEF translations that get wrongly rotated to Y-up by parent
    // Z_UP_TO_Y_UP nodes. i3dm is excluded (marked by tileset.cpp) because
    // its upAxisFix is already applied at the asset root.
    glm::dmat4 upAxisFix(1.0);
    bool skipUpAxisFix = false;
    {
        const auto it = model.extras.find("tilesetio_i3dmFixApplied");
        if (it != model.extras.end() && it->second.isBool() &&
            it->second.getBoolOrDefault(false)) {
            skipUpAxisFix = true;
        }
    }
    if (!skipUpAxisFix) {
        int axis = 1; // Y — glTF default
        
        const auto it = model.extras.find("gltfUpAxis");
        if (it != model.extras.end()) {
            axis = static_cast<int>(it->second.getSafeNumberOrDefault(1.0));
        }
        if (axis == 0) { // X up -> Z up
            upAxisFix = glm::dmat4(
                glm::dvec4(0.0, 0.0, 1.0, 0.0),
                glm::dvec4(0.0, 1.0, 0.0, 0.0),
                glm::dvec4(-1.0, 0.0, 0.0, 0.0),
                glm::dvec4(0.0, 0.0, 0.0, 1.0));
        } else if (axis != 2) { // Y up -> Z up (axis==2 is already Z-up)
            upAxisFix = glm::dmat4(
                glm::dvec4(1.0, 0.0, 0.0, 0.0),
                glm::dvec4(0.0, 0.0, 1.0, 0.0),
                glm::dvec4(0.0, -1.0, 0.0, 0.0),
                glm::dvec4(0.0, 0.0, 0.0, 1.0));
        }
    }

    // Helper: get node local matrix (from matrix or TRS).
    auto nodeLocalMatrix = [](const CesiumGltf::Node& node) {
        glm::dmat4 m(1.0);
        // P37: CesiumGltf::Node::matrix defaults to identity (always 16
        // elements), so size==16 does NOT mean the glTF specified a matrix.
        // If matrix is identity but TRS is non-default, the node used TRS.
        bool matrixIsIdentity = node.matrix.size() == 16;
        if (matrixIsIdentity) {
            for (int i = 0; i < 16; ++i) {
                double expected = (i % 5 == 0) ? 1.0 : 0.0;
                if (node.matrix[static_cast<size_t>(i)] != expected) {
                    matrixIsIdentity = false;
                    break;
                }
            }
        }
        bool trsNonDefault = false;
        if (node.translation.size() == 3 &&
            (node.translation[0] != 0.0 || node.translation[1] != 0.0 ||
             node.translation[2] != 0.0))
            trsNonDefault = true;
        if (node.rotation.size() == 4 &&
            (node.rotation[0] != 0.0 || node.rotation[1] != 0.0 ||
             node.rotation[2] != 0.0 || node.rotation[3] != 1.0))
            trsNonDefault = true;
        if (node.scale.size() == 3 &&
            (node.scale[0] != 1.0 || node.scale[1] != 1.0 ||
             node.scale[2] != 1.0))
            trsNonDefault = true;
        if (!matrixIsIdentity || !trsNonDefault) {
            if (node.matrix.size() == 16) {
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        m[c][r] = node.matrix[static_cast<size_t>(c * 4 + r)];
            }
            return m;
        }
        glm::dvec3 t(0.0);
        glm::dquat q(1.0, 0.0, 0.0, 0.0);
        glm::dvec3 s(1.0);
        if (node.translation.size() == 3)
            t = glm::dvec3(
                node.translation[0], node.translation[1],
                node.translation[2]);
        if (node.rotation.size() == 4)
            q = glm::dquat(
                node.rotation[3], node.rotation[0], node.rotation[1],
                node.rotation[2]);
        if (node.scale.size() == 3)
            s = glm::dvec3(node.scale[0], node.scale[1], node.scale[2]);
        return glm::translate(glm::dmat4(1.0), t) *
               glm::mat4_cast(q) *
               glm::scale(glm::dmat4(1.0), s);
    };

    // Helper: extract a mesh's primitives with node transform applied.
    auto extractMesh = [&](int meshIdx, const glm::dmat4& nodeMatrix) {
        if (meshIdx < 0 ||
            static_cast<size_t>(meshIdx) >= model.meshes.size())
            return;
        // P37: apply Y-up -> Z-up (for non-instanced; identity for i3dm).
        glm::dmat4 worldMat = upAxisFix * nodeMatrix;
        // P37: Detect large ECEF translation (bare glTF RTC_CENTER).
        if (!hasLargeTrans && !skipUpAxisFix) {
            glm::dvec3 t(worldMat[3][0], worldMat[3][1], worldMat[3][2]);
            if (glm::length(t) > 1000.0) {
                hasLargeTrans = true;
                largeTrans = t;
                // Fold into out.tileTransform: out = T(t) * in
                glm::dmat4 inT(1.0);
                for (int cc = 0; cc < 4; ++cc)
                    for (int rr = 0; rr < 4; ++rr)
                        inT[cc][rr] = out.tileTransform[cc * 4 + rr];
                glm::dmat4 transMat(1.0);
                transMat[3] = glm::dvec4(t, 1.0);
                glm::dmat4 combined = transMat * inT;
                for (int cc = 0; cc < 4; ++cc)
                    for (int rr = 0; rr < 4; ++rr)
                        out.tileTransform[cc * 4 + rr] = combined[cc][rr];
            }
        }
        if (hasLargeTrans) {
            // Remove translation from worldMat, keep vertices local
            worldMat[3] = glm::dvec4(0.0, 0.0, 0.0, 1.0);
        }
        const auto& mesh = model.meshes[static_cast<size_t>(meshIdx)];
        for (const auto& prim : mesh.primitives) {
            // P37: accept TRIANGLES and POINTS (pnts point clouds).
            // LINES is only for CESIUM_primitive_outline extension (handled separately).
            const auto mode = prim.mode;
            const bool isTriangles =
                mode == CesiumGltf::MeshPrimitive::Mode::TRIANGLES;
            const bool isPoints =
                mode == CesiumGltf::MeshPrimitive::Mode::POINTS;
            if (!isTriangles && !isPoints)
                continue;

            PrimitiveData pd;
            if (isPoints) {
                pd.primType = 2; // POINTS
            }
            if (!readPositions(
                    model, findAccessor(prim, "POSITION"), pd.positions))
                continue;

            // Apply node transform to positions.
            for (size_t i = 0; i < pd.positions.size(); i += 3) {
                glm::dvec4 p(
                    pd.positions[i], pd.positions[i + 1],
                    pd.positions[i + 2], 1.0);
                glm::dvec4 tp = worldMat * p;
                pd.positions[i] = static_cast<float>(tp.x);
                pd.positions[i + 1] = static_cast<float>(tp.y);
                pd.positions[i + 2] = static_cast<float>(tp.z);
            }

            // Normals, optional (for PBR lighting).
            // Transform by inverse-transpose (rotation only, uniform scale
            // assumed; non-uniform scale on normals is a minor error).
            if (readNormals(
                    model, findAccessor(prim, "NORMAL"), pd.normals)) {
                glm::dmat3 normalMat =
                    glm::transpose(glm::inverse(glm::dmat3(worldMat)));
                for (size_t i = 0; i < pd.normals.size(); i += 3) {
                    glm::dvec3 n(
                        pd.normals[i], pd.normals[i + 1],
                        pd.normals[i + 2]);
                    glm::dvec3 tn = normalMat * n;
                    double len = glm::length(tn);
                    if (len > 1e-12) tn /= len;
                    pd.normals[i] = static_cast<float>(tn.x);
                    pd.normals[i + 1] = static_cast<float>(tn.y);
                    pd.normals[i + 2] = static_cast<float>(tn.z);
                }
            }

            // UVs (TEXCOORD_0), optional.
            readUVs(model, findAccessor(prim, "TEXCOORD_0"), pd.uvs);

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

            // Material color (baseColorFactor) and texture.
            if (prim.material >= 0 &&
                static_cast<size_t>(prim.material) < model.materials.size()) {
                const auto& mat =
                    model.materials[static_cast<size_t>(prim.material)];
                const auto& pbr = mat.pbrMetallicRoughness;
                if (pbr) {
                    if (pbr->baseColorFactor.size() == 4) {
                        pd.color[0] =
                            static_cast<float>(pbr->baseColorFactor[0]);
                        pd.color[1] =
                            static_cast<float>(pbr->baseColorFactor[1]);
                        pd.color[2] =
                            static_cast<float>(pbr->baseColorFactor[2]);
                        pd.color[3] =
                            static_cast<float>(pbr->baseColorFactor[3]);
                    }
                    // Base color texture: material -> texture -> image -> pixels.
                    // Cesium Native already decoded the image; just copy pixels.
                    if (pbr->baseColorTexture &&
                        pbr->baseColorTexture->index >= 0 &&
                        static_cast<size_t>(pbr->baseColorTexture->index) <
                            model.textures.size()) {
                        const auto& tex = model.textures[static_cast<size_t>(
                            pbr->baseColorTexture->index)];
                        if (tex.source >= 0 &&
                            static_cast<size_t>(tex.source) <
                                model.images.size()) {
                            const auto& img = model.images[static_cast<size_t>(
                                tex.source)];
                            if (img.pAsset && img.pAsset->width > 0 &&
                                img.pAsset->height > 0 &&
                                !img.pAsset->pixelData.empty()) {
                                pd.texWidth = img.pAsset->width;
                                pd.texHeight = img.pAsset->height;
                                pd.texPixels = img.pAsset->pixelData;
                            }
                        }
                    }
                }
            }

            // P37: CESIUM_primitive_outline extension - also render outlines as LINES.
            // The extension provides indices for the outline edges.
            if (const auto* pOutline =
                    prim.getExtension<CesiumGltf::ExtensionCesiumPrimitiveOutline>();
                pOutline != nullptr && pOutline->indices >= 0) {
                PrimitiveData outlinePd;
                outlinePd.positions = pd.positions; // copy transformed positions
                outlinePd.primType = 1; // LINES
                // Read outline indices (SCALAR, uint16 or uint32).
                const int outlineIdx = pOutline->indices;
                if (outlineIdx >= 0 &&
                    static_cast<size_t>(outlineIdx) < model.accessors.size()) {
                    // Try uint16 first, then uint32.
                    CesiumGltf::AccessorView<uint16_t> v16(model, outlineIdx);
                    if (v16.status() == CesiumGltf::AccessorViewStatus::Valid) {
                        outlinePd.indices.reserve(static_cast<size_t>(v16.size()));
                        for (int64_t i = 0; i < v16.size(); ++i) {
                            outlinePd.indices.push_back(v16[i]);
                        }
                    } else {
                        CesiumGltf::AccessorView<uint32_t> v32(model, outlineIdx);
                        if (v32.status() == CesiumGltf::AccessorViewStatus::Valid) {
                            outlinePd.indices.reserve(static_cast<size_t>(v32.size()));
                            for (int64_t i = 0; i < v32.size(); ++i) {
                                outlinePd.indices.push_back(v32[i]);
                            }
                        }
                    }
                }
                // Outline uses same bbox as the base primitive.
                for (int i = 0; i < 3; ++i) {
                    outlinePd.bboxMin[i] = pd.bboxMin[i];
                    outlinePd.bboxMax[i] = pd.bboxMax[i];
                }
                // Outlines are typically rendered in a contrasting color (black).
                // Use dark color for visibility.
                outlinePd.color[0] = 0.0f;
                outlinePd.color[1] = 0.0f;
                outlinePd.color[2] = 0.0f;
                outlinePd.color[3] = 1.0f;
                if (!outlinePd.indices.empty()) {
                    out.primitives.push_back(std::move(outlinePd));
                }
            }

            out.primitives.push_back(std::move(pd));
        }
    };

    // Traverse node hierarchy from scenes, accumulating transforms.
    std::function<void(int, const glm::dmat4&)> traverse =
        [&](int nodeIdx, const glm::dmat4& parentMatrix) {
            if (nodeIdx < 0 ||
                static_cast<size_t>(nodeIdx) >= model.nodes.size())
                return;
            const auto& node = model.nodes[static_cast<size_t>(nodeIdx)];
            glm::dmat4 world = parentMatrix * nodeLocalMatrix(node);
            if (node.mesh >= 0)
                extractMesh(node.mesh, world);
            for (int child : node.children)
                traverse(child, world);
        };

    if (!model.scenes.empty()) {
        int sceneIdx = model.scene >= 0 &&
                       static_cast<size_t>(model.scene) < model.scenes.size()
                           ? model.scene
                           : 0;
        for (int rootNode :
             model.scenes[static_cast<size_t>(sceneIdx)].nodes)
            traverse(rootNode, glm::dmat4(1.0));
    } else {
        // No scenes: fall back to all nodes as roots (defensive).
        for (size_t i = 0; i < model.nodes.size(); ++i)
            traverse(static_cast<int>(i), glm::dmat4(1.0));
    }

    // Compute per-primitive bbox from (transformed) positions.
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
