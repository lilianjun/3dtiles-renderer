// P28: internal hardening of cesium-native's tile content converters.
// Not a public SDK header.
//
// Background (upstream CesiumGS/cesium-native#1457, still open):
// some converters can return a GltfConverterResult with an EMPTY model
// and a warnings-only ErrorList (notably CmptToGltfConverter, which
// reports every structural failure via emplaceWarning). The downstream
// TilesetJsonLoader only checks `if (result.errors)` -- and
// ErrorList::operator bool() is hasErrors(), false for warnings-only --
// then unconditionally dereferences `std::move(*result.model)`. That is
// UB; in practice it SIGSEGVs the host process (reproduced locally:
// exit 139 / SIGFPE on corrupt cmpt inputs).
//
// registerHardenedContentConverters() re-registers the magic-dispatched
// converters with wrappers that promote "empty model without a hard
// error" to a real error, so the tile takes the graceful
// TileLoadResult::createFailedResult path instead of crashing.
// This cannot change the behavior of any valid tile: a usable
// conversion always produces a model, so the guard never fires for one.

#pragma once

namespace tiles {

void registerHardenedContentConverters();

} // namespace tiles
