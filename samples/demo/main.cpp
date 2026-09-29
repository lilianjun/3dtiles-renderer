// tiles_renderer SDL3 demo (see docs/adr/0003-sdk-no-sdl.md).
//
// The demo owns the window and the input loop via SDL3, then hands the SDK a
// platform-native window handle + surface size. This is the reference
// integration pattern for host apps on all four target platforms; the SDK
// itself never touches SDL.
//
// P2: renders N frames through the real SDK, optionally saving the last
// frame as PNG (stb_image_write). Headless usage:
//   xvfb-run -a tiles_demo --frames 30 --screenshot out.png

#include <SDL3/SDL.h>
#include <SDL3/SDL_version.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "tiles_renderer/renderer.h"
#include "tiles_renderer/version.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

struct DemoArgs {
    int frames = 30;
    int width = 800;
    int height = 600;
    std::string screenshot; // empty = don't save
};

bool parseArgs(int argc, char** argv, DemoArgs& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char* name, std::string& dst) {
            if (i + 1 >= argc) {
                std::cerr << "[demo] " << name << " needs a value" << std::endl;
                return false;
            }
            dst = argv[++i];
            return true;
        };
        std::string value;
        if (arg == "--frames") {
            if (!needValue("--frames", value)) return false;
            out.frames = std::stoi(value);
        } else if (arg == "--width") {
            if (!needValue("--width", value)) return false;
            out.width = std::stoi(value);
        } else if (arg == "--height") {
            if (!needValue("--height", value)) return false;
            out.height = std::stoi(value);
        } else if (arg == "--screenshot") {
            if (!needValue("--screenshot", out.screenshot)) return false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: tiles_demo [--frames N] [--width W] [--height H] "
                         "[--screenshot out.png]"
                      << std::endl;
            return false;
        } else {
            std::cerr << "[demo] unknown arg: " << arg << std::endl;
            return false;
        }
    }
    if (out.frames < 1) out.frames = 1;
    if (out.width < 1) out.width = 1;
    if (out.height < 1) out.height = 1;
    return true;
}

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

int main(int argc, char** argv) {
    std::cout << "tiles_demo v" << TILES_RENDERER_VERSION
              << " (platform: " << TILES_RENDERER_PLATFORM << ")" << std::endl;
    std::cout << "  SDL3: " << SDL_MAJOR_VERSION << "." << SDL_MINOR_VERSION
              << "." << SDL_MICRO_VERSION << " (demo only; SDK has no SDL dependency)"
              << std::endl;

    DemoArgs args;
    if (!parseArgs(argc, argv, args)) {
        return 1;
    }

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

    SDL_Window* window = SDL_CreateWindow("3dtiles-renderer demo", args.width,
                                          args.height, 0);
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
    config.width = static_cast<std::uint32_t>(w > 0 ? w : args.width);
    config.height = static_cast<std::uint32_t>(h > 0 ? h : args.height);

    int exitCode = 0;
    if (handle == nullptr) {
        // Headless CI (SDL dummy video driver): no native handle exists.
        // The SDK contract requires one, so init is skipped — the demo still
        // exits 0, proving the SDL3 + SDK link path works.
        std::cout << "[demo] no native window handle on this driver; "
                     "SDK init skipped (headless OK)" << std::endl;
    } else if (!tiles_renderer::Renderer::initialize(config)) {
        std::cerr << "[demo] Renderer::initialize failed" << std::endl;
        exitCode = 1;
    } else {
        // Render N successful frames. beginFrame() is non-blocking and
        // returns false while the driver is busy, so retry with pacing like
        // a real main loop instead of counting attempts.
        int rendered = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (rendered < args.frames &&
               std::chrono::steady_clock::now() < deadline) {
            SDL_PumpEvents();
            if (tiles_renderer::Renderer::renderFrame()) {
                ++rendered;
            } else {
                SDL_Delay(4);
            }
        }
        std::cout << "[demo] rendered " << rendered << "/" << args.frames
                  << " frames" << std::endl;
        if (rendered == 0) {
            std::cerr << "[demo] no frame rendered" << std::endl;
            exitCode = 1;
        }

        if (exitCode == 0 && !args.screenshot.empty()) {
            std::vector<std::uint8_t> rgba;
            std::uint32_t sw = 0, sh = 0;
            if (!tiles_renderer::Renderer::readPixels(rgba, sw, sh)) {
                std::cerr << "[demo] readPixels failed" << std::endl;
                exitCode = 1;
            } else if (!stbi_write_png(args.screenshot.c_str(),
                                       static_cast<int>(sw), static_cast<int>(sh),
                                       4, rgba.data(),
                                       static_cast<int>(sw) * 4)) {
                std::cerr << "[demo] failed to write " << args.screenshot
                          << std::endl;
                exitCode = 1;
            } else {
                std::cout << "[demo] screenshot: " << args.screenshot << " ("
                          << sw << "x" << sh << ")" << std::endl;
            }
        }
        tiles_renderer::Renderer::shutdown();
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << "[demo] " << (exitCode == 0 ? "OK" : "FAILED") << std::endl;
    return exitCode;
}
