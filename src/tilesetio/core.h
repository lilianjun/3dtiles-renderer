#pragma once

// tilesetio-core: neutral render data structures.
// This header must NOT include any Filament or cesium-native headers.
// It is the decoupling point between the two.

#include <cstdint>
#include <string>
#include <vector>

namespace tilesetio {

// Neutral PBR material parameters, mirroring glTF 2.0 material.
// The Filament backend maps these to Ubershader MaterialInstance params.
struct MaterialParams {
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float emissiveFactor[3] = {0.0f, 0.0f, 0.0f};

    // Texture indices into TileRenderData::textures (-1 = none).
    int baseColorTexture = -1;
    int metallicRoughnessTexture = -1;
    int normalTexture = -1;
    int emissiveTexture = -1;

    bool doubleSided = false;

    enum class AlphaMode : uint8_t { Opaque = 0, Mask = 1, Blend = 2 };
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;

    std::string name;
};

// Decoded texture image (RGBA8). The core layer decodes, the backend uploads.
struct TextureData {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // 4 * width * height bytes
    std::string mimeType;
};

// A single drawable geometry primitive.
struct PrimitiveData {
    std::vector<float> positions; // 3 floats per vertex, required
    std::vector<float> normals;   // 3 floats per vertex, may be empty
    std::vector<float> texcoords; // 2 floats per vertex, may be empty
    std::vector<uint32_t> indices; // may be empty for non-indexed draw

    uint32_t materialIndex = 0;
    uint32_t nodeIndex = 0; // index into TileRenderData::nodes
};

// A glTF node (transform hierarchy).
struct NodeData {
    // Local transform, column-major 4x4. Already composed from
    // translation/rotation/scale or matrix.
    float transform[16];
    int parent = -1; // -1 = root
};

// Complete render data for one tile.
struct TileRenderData {
    std::vector<PrimitiveData> primitives;
    std::vector<MaterialParams> materials;
    std::vector<TextureData> textures;
    std::vector<NodeData> nodes;

    // Tile-level transform (tile.getTransform() * RTC_CENTER), double precision.
    // Column-major 4x4.
    double tileTransform[16];

    // Human-readable tile ID for logging.
    std::string tileId;
};

} // namespace tilesetio
