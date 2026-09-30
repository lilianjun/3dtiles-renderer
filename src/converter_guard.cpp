// P28: harden cesium-native's tile content converters against the
// empty-model / warnings-only UB (upstream #1457).
//
// Interception point: GltfConverters::registerMagic overwrites any
// previous registration for a magic, so after the SDK's one-time
// registerAllTileContentTypes() call we re-register wrappers for the
// five magic-dispatched converters. The wrapper runs BEFORE the result
// reaches TilesetJsonLoader (whose continuation does the unchecked
// `*result.model` dereference), which is exactly the clean interception
// point the P9 ADR said did not exist -- re-examined in P28, it does:
// no loader fork, no cesium-native patch, no IAssetAccessor byte
// rewriting, no behavior change for valid tiles.

#include "converter_guard.h"

#ifdef TILES_WITH_CESIUM_NATIVE

#include <Cesium3DTilesContent/B3dmToGltfConverter.h>
#include <Cesium3DTilesContent/BinaryToGltfConverter.h>
#include <Cesium3DTilesContent/CmptToGltfConverter.h>
#include <Cesium3DTilesContent/GltfConverterResult.h>
#include <Cesium3DTilesContent/GltfConverters.h>
#include <Cesium3DTilesContent/I3dmToGltfConverter.h>
#include <Cesium3DTilesContent/PntsToGltfConverter.h>
#include <CesiumAsync/Future.h>
#include <CesiumGltfReader/GltfReader.h>

#include <cstddef>
#include <span>
#include <utility>

namespace tiles {
namespace {

// Wraps one cesium-native ConverterFunction. If the wrapped converter
// returns an empty model without a hard error (warnings-only, or even a
// completely clean ErrorList), promote it to a hard error. Downstream,
// TilesetJsonLoader takes the `if (result.errors)` branch and the tile
// fails gracefully via TileLoadResult::createFailedResult instead of
// dereferencing an empty std::optional<Model>.
template <Cesium3DTilesContent::GltfConverters::ConverterFunction Original>
CesiumAsync::Future<Cesium3DTilesContent::GltfConverterResult> hardenedConvert(
    const std::span<const std::byte>& content,
    const CesiumGltfReader::GltfReaderOptions& options,
    const Cesium3DTilesContent::AssetFetcher& assetFetcher) {
  return Original(content, options, assetFetcher)
      .thenImmediately(
          [](Cesium3DTilesContent::GltfConverterResult&& result) {
            // ErrorList::operator bool() == hasErrors(): falsy here means
            // warnings-only at most. An empty model at this point is a
            // converter bug (upstream #1457 for cmpt); without this guard
            // the loader's `*result.model` is UB.
            if (!result.model && !result.errors) {
              result.errors.emplaceError(
                  "Tile content converter returned no model without "
                  "reporting an error; treating the tile as failed.");
            }
            return std::move(result);
          });
}

} // namespace

void registerHardenedContentConverters() {
  using Cesium3DTilesContent::GltfConverters;
  // Same (magic -> converter) mapping as registerAllTileContentTypes(),
  // each wrapped with the empty-model guard. GltfConverters::registerMagic
  // overwrites, so these win deterministically.
  GltfConverters::registerMagic(
      "glTF",
      hardenedConvert<
          Cesium3DTilesContent::BinaryToGltfConverter::convert>);
  GltfConverters::registerMagic(
      "b3dm",
      hardenedConvert<Cesium3DTilesContent::B3dmToGltfConverter::convert>);
  GltfConverters::registerMagic(
      "cmpt",
      hardenedConvert<Cesium3DTilesContent::CmptToGltfConverter::convert>);
  GltfConverters::registerMagic(
      "i3dm",
      hardenedConvert<Cesium3DTilesContent::I3dmToGltfConverter::convert>);
  GltfConverters::registerMagic(
      "pnts",
      hardenedConvert<Cesium3DTilesContent::PntsToGltfConverter::convert>);
}

} // namespace tiles

#else // !TILES_WITH_CESIUM_NATIVE

namespace tiles {
void registerHardenedContentConverters() {}
} // namespace tiles

#endif
