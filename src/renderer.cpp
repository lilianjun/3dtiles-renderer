// tiles_renderer SDK implementation.
//
// P1: validates the host-provided config and proves the cesium-native and
// Filament headers/libraries are wired into the SDK build. Real engine and
// swap-chain creation lands in P2.

#include "tiles_renderer/renderer.h"

#include "tiles_renderer/version.h"

#include <iostream>

#ifdef TILES_WITH_CESIUM_NATIVE
#include <Cesium3DTilesSelection/Tileset.h>
#endif
#ifdef TILES_WITH_FILAMENT
#include <filament/Engine.h>
#endif

namespace tiles_renderer {
namespace {
bool g_initialized = false;
} // namespace

bool Renderer::initialize(const RendererConfig& config) {
    if (config.window == nullptr || config.width == 0 || config.height == 0) {
        std::cerr << "[tiles_renderer] initialize: invalid config "
                     "(need non-null window + nonzero width/height)" << std::endl;
        return false;
    }
    // P1 link/compile probe: name the dependency types so the build proves
    // their headers are visible and their archives are on the link line.
    // (sizeof is compile-time; the link itself is validated by the demo and
    // test binaries that consume this static SDK library.)
#ifdef TILES_WITH_CESIUM_NATIVE
    (void)sizeof(Cesium3DTilesSelection::Tileset);
#endif
#ifdef TILES_WITH_FILAMENT
    (void)sizeof(filament::Engine);
#endif
    std::cout << "[tiles_renderer] initialized " << config.width << "x"
              << config.height << " (SDK v" << TILES_RENDERER_VERSION << ")"
              << std::endl;
    g_initialized = true;
    return true;
}

void Renderer::renderFrame() {
    if (!g_initialized) {
        return;
    }
    // P2: drive the Filament renderer and the cesium-native tileset update.
}

void Renderer::shutdown() {
    g_initialized = false;
    std::cout << "[tiles_renderer] shutdown" << std::endl;
}

const char* Renderer::version() {
    return TILES_RENDERER_VERSION;
}

} // namespace tiles_renderer
