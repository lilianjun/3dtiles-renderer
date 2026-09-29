// P6: SDK lifecycle / fault-injection test (public API, black box).
//
// Exercises, in order:
//   1. calls before initialize()           -> defined no-op / false
//   2. invalid configs                     -> initialize() returns false
//   3. double initialize()                 -> second returns false
//   4. loadTileset() with a missing file   -> false, no crash
//   5. normal render + readPixels          -> true
//   6. calls after shutdown()              -> defined no-op / false
//   7. re-initialize after shutdown()      -> works again
//
// Needs a real X11 window for the valid-config initialize path, so it
// re-execs itself under xvfb-run when no X display is present.

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h> // execvp (xvfb self-wrap)
#endif

#include "tiles_renderer/renderer.h"

namespace {

int g_failures = 0;

// Pump the host event loop: on X11 this flushes the connection (so the
// window actually gets mapped) and lets the WM process the window. A real
// host app pumps events every frame; without this, Filament's beginFrame()
// can keep reporting "swap chain not ready" because the drawable never
// becomes valid. Mirrors the demo's main loop.
void pumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // No interaction needed; just keep the window system alive.
    }
}

// Render up to `want` frames, pumping events and retrying while Filament
// reports the swap chain not ready. Returns the number of frames that
// actually rendered.
int renderFrames(int want, int maxAttempts) {
    int frames = 0;
    for (int i = 0; i < maxAttempts && frames < want; ++i) {
        pumpEvents();
        if (tiles_renderer::Renderer::renderFrame()) {
            ++frames;
        } else {
            SDL_Delay(4);
        }
    }
    return frames;
}

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++g_failures;                                                        \
        } else {                                                                 \
            std::printf("ok   %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
        std::fflush(stdout);                                                     \
    } while (0)

// Extract the platform-native window handle the same way the demo does.
void* nativeHandle(SDL_Window* window) {
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    void* handle = SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    if (handle == nullptr) {
        const Sint64 x11window =
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        handle =
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(x11window));
    }
    return handle;
}

} // namespace

int main(int argc, char** argv) {
    const bool noWrap =
        argc > 1 && std::string(argv[1]) == "--no-xvfb-wrap";
    if (!noWrap && std::getenv("DISPLAY") == nullptr) {
        // No X server: re-exec under xvfb-run (same pattern as the python
        // screenshot tests).
        const char* xvfbArgs[] = {"xvfb-run",
                                  "-a",
                                  "-s",
                                  "-screen 0 1024x768x24",
                                  argv[0],
                                  "--no-xvfb-wrap",
                                  nullptr};
        // NOLINTNEXTLINE: execvp only returns on failure.
        execvp("xvfb-run", const_cast<char* const*>(xvfbArgs));
        std::perror("execvp xvfb-run");
        return 2;
    }

    using tiles_renderer::Renderer;
    using tiles_renderer::RendererConfig;

    std::printf("[lifecycle] pre-init calls (must be safe no-ops)\n");
    CHECK(Renderer::renderFrame() == false);
    {
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0, h = 0;
        CHECK(Renderer::readPixels(rgba, w, h) == false);
    }
    CHECK(Renderer::loadTileset("/nonexistent/tileset.json") == false);
    CHECK(Renderer::renderedTileCount() == -1);
    Renderer::setOrbitCamera(10.0f, 10.0f, 10.0f); // must not crash
    Renderer::shutdown();                          // must not crash
    std::printf("[lifecycle] pre-init done\n");

    std::printf("[lifecycle] invalid configs\n");
    {
        RendererConfig bad{};
        CHECK(Renderer::initialize(bad) == false); // null window, zero size
        bad.window = reinterpret_cast<void*>(0x1);
        CHECK(Renderer::initialize(bad) == false); // zero size
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("lifecycle", 800, 600, 0);
    if (window == nullptr) {
        std::printf("FAIL: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    void* handle = nativeHandle(window);
    std::printf("[lifecycle] native handle: %p\n", handle);
    CHECK(handle != nullptr);

    std::printf("[lifecycle] double initialize\n");
    {
        RendererConfig cfg;
        cfg.window = static_cast<tiles_renderer::NativeWindowHandle>(handle);
        cfg.width = 800;
        cfg.height = 600;
        CHECK(Renderer::initialize(cfg) == true);
        CHECK(Renderer::initialize(cfg) == false); // already initialized
    }

    std::printf("[lifecycle] load failure is graceful\n");
    CHECK(Renderer::loadTileset("/nonexistent/tileset.json") == false);
    CHECK(Renderer::renderedTileCount() == -1); // no tileset loaded

    std::printf("[lifecycle] normal render path\n");
    CHECK(renderFrames(5, 300) == 5);
    {
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0, h = 0;
        CHECK(Renderer::readPixels(rgba, w, h) == true);
        CHECK(w == 800 && h == 600);
        CHECK(rgba.size() == static_cast<std::size_t>(800) * 600 * 4);
    }

    std::printf("[lifecycle] post-shutdown calls (must be safe)\n");
    Renderer::shutdown();
    CHECK(Renderer::renderFrame() == false);
    {
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0, h = 0;
        CHECK(Renderer::readPixels(rgba, w, h) == false);
    }
    CHECK(Renderer::loadTileset("/nonexistent/tileset.json") == false);
    CHECK(Renderer::renderedTileCount() == -1);
    Renderer::setOrbitCamera(10.0f, 10.0f, 10.0f); // must not crash
    Renderer::shutdown();                          // double shutdown: no crash

    std::printf("[lifecycle] re-initialize after shutdown\n");
    {
        RendererConfig cfg;
        cfg.window = static_cast<tiles_renderer::NativeWindowHandle>(handle);
        cfg.width = 800;
        cfg.height = 600;
        CHECK(Renderer::initialize(cfg) == true);
        CHECK(renderFrames(3, 300) == 3);
        Renderer::shutdown();
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (g_failures == 0) {
        std::printf("PASS: lifecycle\n");
        return 0;
    }
    std::printf("FAIL: lifecycle (%d failures)\n", g_failures);
    return 1;
}
