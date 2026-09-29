// P12: host-integration check — exercises the SDK's public API the way a
// real host app would (initialize -> render -> resize -> load -> render ->
// shutdown), including the P12 additions Renderer::resize() and
// Renderer::lastError().
//
// Needs a real X11 window, so it re-execs itself under xvfb-run when no X
// display is present (same pattern as lifecycle.cpp). argv[1] is a directory
// containing a loadable tileset.json (passed by CMake).
//
// Uses only the public header <tiles_renderer/renderer.h> — this file is the
// compile-time proof that the integration guide's Linux snippet is real.

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h> // execvp (xvfb self-wrap)
#endif

#include "tiles_renderer/renderer.h"

namespace {

int g_failures = 0;

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

void pumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
    }
}

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

// Same native-handle extraction as the demo.
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
    // argv: [--no-xvfb-wrap] [fixtureDir]
    int argi = 1;
    const bool noWrap =
        argi < argc && std::string(argv[argi]) == "--no-xvfb-wrap";
    if (noWrap) {
        ++argi;
    }
    const std::string fixtureDir = (argi < argc) ? argv[argi] : "";
    if (!noWrap && std::getenv("DISPLAY") == nullptr) {
        const char* xvfbArgs[] = {"xvfb-run",
                                  "-a",
                                  "-s",
                                  "-screen 0 1024x768x24",
                                  argv[0],
                                  "--no-xvfb-wrap",
                                  fixtureDir.c_str(),
                                  nullptr};
        // NOLINTNEXTLINE: execvp only returns on failure.
        execvp("xvfb-run", const_cast<char* const*>(xvfbArgs));
        std::perror("execvp xvfb-run");
        return 2;
    }

    using tiles_renderer::Renderer;
    using tiles_renderer::RendererConfig;

    std::printf("[host] resize/lastError before init\n");
    CHECK(Renderer::resize(400, 300) == false);
    CHECK(!Renderer::lastError().empty());
    CHECK(Renderer::loadTileset("/nonexistent/tileset.json") == false);
    CHECK(!Renderer::lastError().empty());

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("host_integration", 800, 600, 0);
    if (window == nullptr) {
        std::printf("FAIL: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    void* handle = nativeHandle(window);
    CHECK(handle != nullptr);

    std::printf("[host] initialize + initial render\n");
    {
        RendererConfig cfg;
        cfg.window = static_cast<tiles_renderer::NativeWindowHandle>(handle);
        cfg.width = 800;
        cfg.height = 600;
        CHECK(Renderer::initialize(cfg) == true);
        CHECK(Renderer::lastError().empty());
    }
    CHECK(renderFrames(5, 300) == 5);

    std::printf("[host] resize 800x600 -> 400x300\n");
    CHECK(Renderer::resize(400, 300) == true);
    CHECK(Renderer::lastError().empty());
    CHECK(Renderer::resize(400, 300) == true); // no-op, same size
    CHECK(Renderer::resize(0, 300) == false);  // invalid
    CHECK(!Renderer::lastError().empty());
    CHECK(renderFrames(5, 300) == 5);
    {
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0, h = 0;
        CHECK(Renderer::readPixels(rgba, w, h) == true);
        CHECK(w == 400 && h == 300);
        CHECK(rgba.size() == static_cast<std::size_t>(400) * 300 * 4);
    }

    if (!fixtureDir.empty()) {
        std::printf("[host] loadTileset failure reports why\n");
        const std::string bad = fixtureDir + "/../__no_such_dir__/tileset.json";
        CHECK(Renderer::loadTileset(bad) == false);
        const std::string err = Renderer::lastError();
        CHECK(!err.empty());
        std::printf("[host] lastError: %s\n", err.c_str());
        CHECK(err.find("__no_such_dir__") != std::string::npos);

        std::printf("[host] loadTileset success clears the error\n");
        const std::string good = fixtureDir + "/tileset.json";
        Renderer::setOrbitCamera(35.0f, 25.0f, 30.0f);
        CHECK(Renderer::loadTileset(good) == true);
        CHECK(Renderer::lastError().empty());
        CHECK(renderFrames(30, 600) >= 1);
        CHECK(Renderer::renderedTileCount() >= 0);
    } else {
        std::printf("[host] no fixture dir given; skipping loadTileset\n");
    }

    std::printf("[host] resize back + shutdown\n");
    CHECK(Renderer::resize(800, 600) == true);
    CHECK(renderFrames(3, 300) == 3);
    Renderer::shutdown();
    CHECK(Renderer::resize(100, 100) == false); // post-shutdown
    CHECK(!Renderer::lastError().empty());

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (g_failures != 0) {
        std::printf("[host] %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("[host] all checks passed\n");
    return 0;
}
