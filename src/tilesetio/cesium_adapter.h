#pragma once

// tilesetio cesium-native adapter: converts CesiumGltf::Model to
// tilesetio::TileRenderData without going through GLB bytes.
// This header may include cesium-native headers, but NOT Filament headers.

#include "core.h"

#include <CesiumGltf/Model.h>

#include <string>

namespace tilesetio {

// Convert a cesium-native Model to neutral render data.
// Extracts raw memory data (positions, indices, colors) via AccessorView.
// No GLB serialization.
// - tileTransform receives the tile-level transform caller computed
//   (tile.getTransform() * RTC_CENTER), column-major, double.
TileRenderData convertModel(
    const CesiumGltf::Model& model,
    const double tileTransform[16],
    const std::string& tileId,
    const double localOrigin[3] = nullptr,
    bool isChildTile = false,
    bool enableShowOutline = true,
    const float outlineColor[3] = nullptr);

} // namespace tilesetio
