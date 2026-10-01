#pragma once

// tilesetio cesium-native adapter: converts CesiumGltf::Model to
// tilesetio::TileRenderData without going through GLB bytes.
// This header may include cesium-native headers, but NOT Filament headers.

#include "core.h"

#include <CesiumGltf/Model.h>

#include <string>

namespace tilesetio {

// Convert a cesium-native Model to neutral render data.
// - Reads vertex attributes directly via AccessorView (no GLB round-trip).
// - Extracts PBR material params (no shader compilation here).
// - Flattens node hierarchy to NodeData list.
// - tileTransformOut receives the tile-level transform caller computed
//   (tile.getTransform() * RTC_CENTER), column-major, double.
TileRenderData convertModel(
    const CesiumGltf::Model& model,
    const double tileTransform[16],
    const std::string& tileId);

} // namespace tilesetio
