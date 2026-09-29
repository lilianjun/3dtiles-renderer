// tiles_renderer SDL3 demo (see docs/adr/0003-sdk-no-sdl.md).
//
// The demo owns the window and the input loop via SDL3, then hands the SDK a
// platform-native window handle + surface size. This is the reference
// integration pattern for host apps on all four target platforms; the SDK
// itself never touches SDL.

#include <SDL3/SDL.h>
#include <SDL3/SDL_version.h>

#include <cstdint>
#include <iostream>

#include "tiles_renderer/renderer.h"
#include "tiles_renderer/version.h"

namespace {

// Extract the platform-native window handle for the SDK.
tiles_renderer::NativeWindowHandle nativeHandle(SDL_Window* window) {
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
#if defined(SDL_PLATFORM_WINDOWS)
    return static_cast<tiles_renderer::NativeWindowHandle>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
#elif defined(SDL_PLATFORM_ANDROID)
    return static_cast<tiles_renderer::NativeWindowHandle>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
#elif defined(SDL_PLATFORM_IOS)
    return static_cast<tiles_renderer::NativeWindowHandle>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr));
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
    return "#canvas";
#else
    // Linux/macOS desktop: prefer Wayland surface, fall back to X11 window.
    // Filament's createSwapChain consumes these as void*.
    void* handle =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    if (handle == nullptr) {
        const Sint64 x11window =
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(x11window));
    }
    return static_cast<tiles_renderer::NativeWindowHandle>(handle);
#endif
}

} // namespace

int main() {
    std::cout << "tiles_demo v" << TILES_RENDERER_VERSION
              << " (platform: " << TILES_RENDERER_PLATFORM << ")" << std::endl;
    std::cout << "  SDL3: " << SDL_MAJOR_VERSION << "." << SDL_MINOR_VERSION
              << "." << SDL_MICRO_VERSION << " (demo only; SDK has no SDL dependency)"
              << std::endl;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        // Headless environment (no X11/Wayland, e.g. CI): SDL3's dummy and
        // offscreen video drivers are opt-in only — they are not auto-selected.
        // Fall back to the dummy driver explicitly so the demo still runs.
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::cerr << "[demo] SDL_Init failed: " << SDL_GetError() << std::endl;
            return 1;
        }
        std::cout << "[demo] headless: using SDL dummy video driver" << std::endl;
    }

    SDL_Window* window = SDL_CreateWindow("3dtiles-renderer demo", 1280, 720, 0);
    if (window == nullptr) {
        std::cerr << "[demo] SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    const auto handle = nativeHandle(window);

    tiles_renderer::RendererConfig config;
    config.window = handle;
    config.width = static_cast<std::uint32_t>(w > 0 ? w : 1280);
    config.height = static_cast<std::uint32_t>(h > 0 ? h : 720);

    if (handle == nullptr) {
        // Headless CI (SDL dummy video driver): no native handle exists.
        // The SDK contract requires one, so init is skipped — the demo still
        // exits 0, proving the SDL3 + SDK link path works.
        std::cout << "[demo] no native window handle on this driver; "
                     "SDK init skipped (headless OK)" << std::endl;
    } else if (tiles_renderer::Renderer::initialize(config)) {
        tiles_renderer::Renderer::renderFrame();
        tiles_renderer::Renderer::shutdown();
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << "[demo] OK" << std::endl;
    return 0;
}
