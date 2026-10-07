#pragma once

// tilesetio-core: neutral render data structures.
// This header must NOT include any Filament or cesium-native headers.
// It is the decoupling point between the two.
//
// The adapter extracts RAW MEMORY DATA (positions, indices, colors) from
// the CesiumGltf::Model. The backend consumes the raw data via standard
// Filament API (VertexBuffer, IndexBuffer, RenderableManager). No glTF,
// no GLB round-trip, no gltfio.

#include <cstdint>
#include <string>
#include <vector>

namespace tilesetio {

// A single drawable geometry primitive: raw memory data.
struct PrimitiveData {
    std::vector<float> positions;    // 3 floats per vertex, required
    std::vector<float> normals;      // 3 floats per vertex, optional (for PBR lighting)
    std::vector<float> uvs;          // 2 floats per vertex, optional (TEXCOORD_0)
    std::vector<float> colors;       // 3 floats per vertex, optional (COLOR_0, for pnts)
    std::vector<uint32_t> indices;   // may be empty for non-indexed draw
    float color[4] = {1, 1, 1, 1};   // solid color (from material baseColorFactor)
    float bboxMin[3] = {0, 0, 0};    // axis-aligned bbox in local space
    float bboxMax[3] = {0, 0, 0};
    // Texture (from material baseColorTexture), if present.
    // Raw RGBA pixels decoded by Cesium Native; backend uploads to GPU.
    int texWidth = 0;
    int texHeight = 0;
    std::vector<std::byte> texPixels; // RGBA, texWidth*texHeight*4 bytes
    // P37: primitive type for rendering (0=TRIANGLES, 1=LINES, 2=POINTS).
    // TRIANGLES is default; LINES for CESIUM_primitive_outline; POINTS for pnts.
    int primType = 0;
    // P37: true if this was a POINTS primitive expanded to billboard quads.
    // Backend uses point_billboard material (vertex shader billboarding).
    bool isBillboard = false;
};

// Complete render data for one tile.
struct TileRenderData {
    std::vector<PrimitiveData> primitives;

    // Tile-level transform (tile.getTransform() * RTC_CENTER), double precision.
    // Column-major 4x4. Applied by the orchestrator to the renderable entities.
    double tileTransform[16];

    // Human-readable tile ID for logging.
    std::string tileId;
};

} // namespace tilesetio
