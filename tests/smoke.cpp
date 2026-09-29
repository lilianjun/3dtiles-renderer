// Smoke test: verifies the toolchain, the generated version header, the SDK
// static library links, and that a test binary can build + run under CTest.
// (SDL3 is linked here only as a consumer — the SDK itself has no SDL
// dependency. See docs/adr/0003-sdk-no-sdl.md.)

#include <cassert>
#include <iostream>
#include <string>

#include "tiles_renderer/renderer.h"
#include "tiles_renderer/version.h"

#ifdef TILES_WITH_SDL3
#include <SDL3/SDL_version.h>
#endif

int main() {
    static_assert(__cplusplus >= 202002L, "C++20 required");

    std::cout << "[smoke] version=" << TILES_RENDERER_VERSION
              << " platform=" << TILES_RENDERER_PLATFORM << std::endl;

    assert(TILES_RENDERER_VERSION[0] != '\0');
    assert(TILES_RENDERER_PLATFORM[0] != '\0');

    // The SDK must reject an invalid config (null window / zero size).
    tiles_renderer::RendererConfig bad;
    assert(tiles_renderer::Renderer::initialize(bad) == false);

    assert(std::string(tiles_renderer::Renderer::version()) == TILES_RENDERER_VERSION);

#ifdef TILES_WITH_SDL3
    std::cout << "[smoke] SDL3 (test consumer): " << SDL_MAJOR_VERSION << "."
              << SDL_MINOR_VERSION << "." << SDL_MICRO_VERSION << std::endl;
#endif

    std::cout << "[smoke] OK" << std::endl;
    return 0;
}
