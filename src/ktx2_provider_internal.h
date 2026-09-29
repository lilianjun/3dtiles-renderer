// P25: internal factory for the KTX2 texture provider.
// On cesium builds (Linux) this creates the libktx-backed provider
// (src/ktx2_libktx_provider.cpp); elsewhere tileset.cpp uses filament's
// createKtx2Provider directly. Not a public SDK header.

#pragma once

namespace filament {
class Engine;
namespace gltfio {
class TextureProvider;
} // namespace gltfio
} // namespace filament

namespace tiles {

// Returns nullptr if KTX2 is not supported on this build; the caller then
// simply does not register an "image/ktx2" provider.
filament::gltfio::TextureProvider* createLibktxKtx2Provider(
    filament::Engine* engine);

} // namespace tiles
