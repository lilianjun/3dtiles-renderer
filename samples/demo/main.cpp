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
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#ifdef __linux__
#include <unistd.h> // P19: sysconf for --print-rss
#endif
#include <vector>

#include "tiles_renderer/renderer.h"
#include "tiles_renderer/version.h"
#include "trajectory.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

struct DemoArgs {
    int frames = 30;
    bool framesExplicit = false; // set when --frames was passed
    int width = 800;
    int height = 600;
    std::string screenshot; // empty = don't save
    std::string tileset;    // empty = built-in small tileset next to the exe
    bool noTileset = false; // P2 mode: render the fixed clear + triangle
    // P16: deterministic camera trajectory replay (ADR-0014). When set, the
    // orbit camera is driven per successful frame from the CSV keyframes;
    // --frames defaults to the trajectory length. Mouse orbit is disabled
    // in this mode so the replay stays bit-identical.
    std::string trajectory; // empty = fixed camera mode
    std::string frameDir;   // empty = don't dump per-frame PNGs
    int warmup = 60;        // successful frames at the first keyframe camera
                            // before the trajectory starts (lets async tile
                            // loading settle; not dumped, not counted)
    bool stats = false;     // P17: print per-frame TileStats to stdout
    bool printSelected = false; // P20: print per-frame selected tile IDs
    bool noIbl = false;     // P26: disable the default image-based lighting
                            // (renders with the P3 directional sun only)
    // P18: weak-network test hooks (dev/test only, not for production use).
    // --until-loaded N renders up to N frames but stops early once the
    // tileset has settled (loading == 0 && loaded > 0 for 20 consecutive
    // frames); exits 1 if the budget is exhausted first. --exit-on-loading
    // aborts (through the normal shutdown path) on the first frame where
    // any tile load is in flight — this exercises mid-load teardown.
    int untilLoaded = 0;  // 0 = disabled; >0 = max frames for settle polling
    bool exitOnLoading = false;
    // P18: on the first frame with any tile load in flight, push the camera
    // far out (deselecting child tiles) and keep rendering. Exercises
    // request cancellation/drain for tiles that fall out of selection.
    bool zoomOutOnLoading = false;
    // P19: tile cache budget in bytes (0 = default 512MB); --print-rss
    // prints process RSS (KB) after init and after the last frame.
    std::int64_t cacheBudget = 0;
    bool printRss = false;
    // P22: mid-run tileset switching (test/dev only). Each --switch-tileset
    // PATH appends a switch; the matching --switch-at-frame N (logical
    // rendered-frame index, same order) fires Renderer::loadTileset(PATH)
    // before that frame. A failed switch is logged ([switch] ok=0 ...) and
    // does NOT abort the run, so tests can assert the old tileset survives.
    std::vector<std::string> switchTilesets;
    std::vector<int> switchAtFrames;
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
            out.framesExplicit = true;
        } else if (arg == "--width") {
            if (!needValue("--width", value)) return false;
            out.width = std::stoi(value);
        } else if (arg == "--height") {
            if (!needValue("--height", value)) return false;
            out.height = std::stoi(value);
        } else if (arg == "--screenshot") {
            if (!needValue("--screenshot", out.screenshot)) return false;
        } else if (arg == "--tileset") {
            if (!needValue("--tileset", out.tileset)) return false;
        } else if (arg == "--no-tileset") {
            out.noTileset = true;
        } else if (arg == "--trajectory") {
            if (!needValue("--trajectory", out.trajectory)) return false;
        } else if (arg == "--frame-dir") {
            if (!needValue("--frame-dir", out.frameDir)) return false;
        } else if (arg == "--warmup") {
            if (!needValue("--warmup", value)) return false;
            out.warmup = std::stoi(value);
        } else if (arg == "--stats") {
            out.stats = true;
        } else if (arg == "--print-selected") {
            out.printSelected = true;
        } else if (arg == "--no-ibl") {
            out.noIbl = true;
        } else if (arg == "--until-loaded") {
            if (!needValue("--until-loaded", value)) return false;
            out.untilLoaded = std::stoi(value);
            out.frames = out.untilLoaded;
            out.framesExplicit = true;
        } else if (arg == "--exit-on-loading") {
            out.exitOnLoading = true;
        } else if (arg == "--zoom-out-on-loading") {
            out.zoomOutOnLoading = true;
        } else if (arg == "--cache-budget") {
            if (!needValue("--cache-budget", value)) return false;
            out.cacheBudget = std::stoll(value);
        } else if (arg == "--print-rss") {
            out.printRss = true;
        } else if (arg == "--switch-tileset") {
            std::string v;
            if (!needValue("--switch-tileset", v)) return false;
            out.switchTilesets.push_back(v);
            out.switchAtFrames.push_back(-1); // must be set via --switch-at-frame
        } else if (arg == "--switch-at-frame") {
            std::string v;
            if (!needValue("--switch-at-frame", v)) return false;
            if (out.switchAtFrames.empty() || out.switchAtFrames.back() != -1) {
                std::cerr << "[demo] --switch-at-frame needs a preceding "
                             "--switch-tileset" << std::endl;
                return false;
            }
            out.switchAtFrames.back() = std::stoi(v);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: tiles_demo [--frames N] [--width W] [--height H] "
                         "[--screenshot out.png] [--tileset path-or-url] "
                         "[--no-tileset] [--trajectory keys.csv] "
                         "[--frame-dir dir] [--warmup N] [--stats] "
                         "[--print-selected] "
                         "[--until-loaded N] [--exit-on-loading] "
                         "[--zoom-out-on-loading] [--cache-budget BYTES] "
                         "[--print-rss] "
                         "[--switch-tileset PATH --switch-at-frame N]... "
                         "[--no-ibl]"
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
    for (int f : out.switchAtFrames) {
        if (f < 0) {
            std::cerr << "[demo] every --switch-tileset needs a "
                         "--switch-at-frame" << std::endl;
            return false;
        }
    }
    return true;
}

// Resolve the built-in P3 tileset: <exe-dir>/p3_box_tileset/tileset.json
// (copied there by CMake). Returns "" when it cannot be determined.
std::string builtinTilesetPath() {
    const char* base = SDL_GetBasePath();
    if (base == nullptr) {
        return "";
    }
    std::string path = base;
    SDL_free(const_cast<char*>(base));
    path += "p3_box_tileset/tileset.json";
    return path;
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
        // P26: IBL toggle for A/B pixel tests (default is on).
        if (args.noIbl) {
            tiles_renderer::Renderer::setIblEnabled(false);
        }
        // P19: RSS helper (Linux /proc/self/statm; dev/test only).
        auto readRssKb = []() -> long {
#ifdef __linux__
            FILE* f = std::fopen("/proc/self/statm", "r");
            if (f == nullptr) {
                return -1;
            }
            long size = 0, resident = 0;
            if (std::fscanf(f, "%ld %ld", &size, &resident) != 2) {
                resident = -1;
            }
            std::fclose(f);
            if (resident < 0) {
                return -1;
            }
            return resident * (sysconf(_SC_PAGESIZE) / 1024);
#else
            return -1; // unsupported platform
#endif
        };
        if (args.printRss) {
            std::cout << "[rss] start=" << readRssKb() << "KB" << std::endl;
        }
        // P19: tile cache budget (0 = leave the SDK default alone).
        if (args.cacheBudget > 0) {
            tiles_renderer::Renderer::setMaxCachedBytes(args.cacheBudget);
            std::cout << "[demo] cache budget: " << args.cacheBudget << " bytes"
                      << std::endl;
        }
        // P3: load a tileset unless --no-tileset was given. Default is the
        // built-in tiny tileset shipped next to the demo binary.
        float yawDeg = 30.0f, pitchDeg = 18.0f, distance = 20.0f;
        bool tilesetLoaded = false;
        if (!args.noTileset) {
            const std::string tilesetPath =
                args.tileset.empty() ? builtinTilesetPath() : args.tileset;
            if (tilesetPath.empty()) {
                std::cerr << "[demo] cannot determine the built-in tileset "
                             "path (pass --tileset explicitly)"
                          << std::endl;
                exitCode = 1;
            } else if (!tiles_renderer::Renderer::loadTileset(tilesetPath)) {
                std::cerr << "[demo] failed to load tileset: " << tilesetPath
                          << std::endl;
                exitCode = 1;
            } else {
                tilesetLoaded = true;
                std::cout << "[demo] tileset: " << tilesetPath << std::endl;
            }
        }
        // P16: trajectory replay (ADR-0014). The player maps a logical frame
        // index -> orbit camera; the frame index advances only on successful
        // renderFrame() calls, so the replay never depends on wall clock.
        // Mouse orbit is disabled in this mode to keep it bit-identical.
        bool trajectoryMode = !args.trajectory.empty();
        std::optional<trajectory::Player> player;
        if (trajectoryMode) {
            player = trajectory::Player::loadCsv(args.trajectory);
            std::cout << "[demo] trajectory: " << args.trajectory << " ("
                      << player->keyframeCount() << " keyframes, "
                      << player->lastFrame() + 1 << " frames)" << std::endl;
            if (!args.framesExplicit) {
                args.frames = player->lastFrame() + 1;
            }
            if (!args.frameDir.empty()) {
                // Best-effort mkdir; failure surfaces at first PNG write.
                std::error_code ec;
                std::filesystem::create_directories(args.frameDir, ec);
            }
        }
        tiles_renderer::Renderer::setOrbitCamera(yawDeg, pitchDeg, distance);

        auto dumpFramePng = [&](int frameIndex) -> bool {
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "frame_%04d.png", frameIndex);
            const std::string name =
                args.frameDir + "/" + std::string(suffix);
            std::vector<std::uint8_t> rgba;
            std::uint32_t sw = 0, sh = 0;
            if (!tiles_renderer::Renderer::readPixels(rgba, sw, sh)) {
                std::cerr << "[demo] readPixels failed at frame " << frameIndex
                          << std::endl;
                return false;
            }
            if (!stbi_write_png(name.c_str(), static_cast<int>(sw),
                                static_cast<int>(sh), 4, rgba.data(),
                                static_cast<int>(sw) * 4)) {
                std::cerr << "[demo] failed to write " << name << std::endl;
                return false;
            }
            return true;
        };

        // Render one successful frame; returns false on deadline expiry.
        // readPixels per frame is heavier than the API intends for production
        // use, but this is a dev/test tool exporting a trajectory, not a
        // shipping frame loop.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(180);
        bool dragging = false;
        auto pumpEvents = [&](bool allowOrbit) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    return false;
                } else if (allowOrbit && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                           event.button.button == SDL_BUTTON_LEFT) {
                    dragging = true;
                } else if (allowOrbit && event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                           event.button.button == SDL_BUTTON_LEFT) {
                    dragging = false;
                } else if (allowOrbit && event.type == SDL_EVENT_MOUSE_MOTION && dragging) {
                    yawDeg -= static_cast<float>(event.motion.xrel) * 0.4f;
                    pitchDeg += static_cast<float>(event.motion.yrel) * 0.4f;
                    if (pitchDeg > 85.0f) pitchDeg = 85.0f;
                    if (pitchDeg < -85.0f) pitchDeg = -85.0f;
                    tiles_renderer::Renderer::setOrbitCamera(
                        yawDeg, pitchDeg, distance);
                } else if (allowOrbit && event.type == SDL_EVENT_MOUSE_WHEEL) {
                    distance *= (event.wheel.y > 0) ? 0.9f : 1.1f;
                    if (distance < 2.0f) distance = 2.0f;
                    if (distance > 200.0f) distance = 200.0f;
                    tiles_renderer::Renderer::setOrbitCamera(
                        yawDeg, pitchDeg, distance);
                }
            }
            return std::chrono::steady_clock::now() < deadline;
        };

        // P16 warmup: hold the first-keyframe camera so async tile loading
        // settles before the recorded trajectory starts. Not dumped, not
        // counted toward the trajectory frame index.
        int rendered = 0;
        if (trajectoryMode && args.warmup > 0) {
            const auto pose = player->at(0);
            tiles_renderer::Renderer::setOrbitCamera(
                static_cast<float>(pose.yawDeg),
                static_cast<float>(pose.pitchDeg),
                static_cast<float>(pose.distance));
            int warmed = 0;
            bool alive = true;
            while (warmed < args.warmup && alive) {
                alive = pumpEvents(false);
                if (tiles_renderer::Renderer::renderFrame()) {
                    ++warmed;
                } else {
                    SDL_Delay(4);
                }
            }
            std::cout << "[demo] trajectory warmup: " << warmed << "/"
                      << args.warmup << " frames" << std::endl;
            if (warmed < args.warmup) {
                std::cerr << "[demo] warmup hit the deadline" << std::endl;
                exitCode = 1;
            }
        }

        // Render N successful frames. beginFrame() is non-blocking and
        // returns false while the driver is busy, so retry with pacing like
        // a real main loop instead of counting attempts.
        //
        // P3: simple orbit control — drag with the left mouse button to
        // orbit, mouse wheel to zoom. (Disabled in trajectory mode.)
        bool alive = exitCode == 0;
        // P18: consecutive settled frames needed before --until-loaded
        // declares the tileset stable (guards transient queue-empty gaps
        // between the tileset.json fetch and the child tile requests).
        int settleStreak = 0;
        bool settled = false;
        const int kSettleFrames = 20;
        bool zoomedOut = false; // P18: --zoom-out-on-loading fired once
        while (rendered < args.frames && alive) {
            alive = pumpEvents(!trajectoryMode);
            if (trajectoryMode) {
                const auto pose = player->at(rendered);
                tiles_renderer::Renderer::setOrbitCamera(
                    static_cast<float>(pose.yawDeg),
                    static_cast<float>(pose.pitchDeg),
                    static_cast<float>(pose.distance));
            }
            // P22: mid-run tileset switch. Fires before the frame's render
            // so frame N is the first frame of the new tileset. Failures are
            // logged, never fatal (the SDK keeps the old tileset).
            for (size_t s = 0; s < args.switchTilesets.size(); ++s) {
                if (args.switchAtFrames[s] == rendered) {
                    const bool ok = tiles_renderer::Renderer::loadTileset(
                        args.switchTilesets[s]);
                    std::cout << "[switch] frame=" << rendered
                              << " ok=" << (ok ? 1 : 0);
                    if (!ok) {
                        std::cout << " error="
                                  << tiles_renderer::Renderer::lastError();
                    }
                    std::cout << std::endl;
                }
            }
            if (tiles_renderer::Renderer::renderFrame()) {
                if (!args.frameDir.empty() && !dumpFramePng(rendered)) {
                    exitCode = 1;
                    break;
                }
                // P18: settle polling / mid-load abort hooks (dev/test).
                bool stopEarly = false;
                if (args.untilLoaded > 0 || args.exitOnLoading ||
                    args.zoomOutOnLoading) {
                    const auto st = tiles_renderer::Renderer::tileStats();
                    if (args.exitOnLoading && st.tilesLoading > 0) {
                        std::cout << "[demo] abort: load in flight at frame "
                                  << rendered << std::endl;
                        stopEarly = true;
                    }
                    if (args.zoomOutOnLoading && !zoomedOut &&
                        st.tilesLoading > 0) {
                        // Push the camera far out with the current yaw/pitch:
                        // child tiles drop out of selection while their
                        // requests are in flight. 2000 is calibrated for the
                        // p3 fixture (root geometricError 16, children 0):
                        // root SSE falls below maxScreenSpaceError there.
                        tiles_renderer::Renderer::setOrbitCamera(
                            yawDeg, pitchDeg, 2000.0f);
                        zoomedOut = true;
                        std::cout << "[demo] zoomed out at frame " << rendered
                                  << std::endl;
                    }
                    if (args.untilLoaded > 0) {
                        if (st.tilesLoading == 0 && st.tilesLoaded > 0) {
                            if (++settleStreak >= kSettleFrames) {
                                std::cout
                                    << "[demo] settled: frames=" << rendered
                                    << " loaded=" << st.tilesLoaded
                                    << " failed=" << st.tilesFailed
                                    << std::endl;
                                settled = true;
                                stopEarly = true;
                            }
                        } else {
                            settleStreak = 0;
                        }
                    }
                }
                if (args.stats) {
                    // P17: per-frame streaming diagnostics (dev/test HUD).
                    // Format is stable for tests/stats_test.py to parse.
                    const auto st =
                        tiles_renderer::Renderer::tileStats();
                    std::cout << "[stats] frame=" << rendered
                              << " selected=" << st.selectedTiles
                              << " loading=" << st.tilesLoading
                              << " loaded=" << st.tilesLoaded
                              << " failed=" << st.tilesFailed
                              << " bytes=" << st.bytesLoaded << std::endl;
                }
                if (args.printSelected) {
                    // P20: per-frame selected tile IDs (frustum/LOD tests).
                    // Format: [selected] frame=N ids=<id1>,<id2>,...
                    const auto ids =
                        tiles_renderer::Renderer::selectedTileIds();
                    std::cout << "[selected] frame=" << rendered << " ids=";
                    for (size_t i = 0; i < ids.size(); ++i) {
                        if (i > 0) std::cout << ",";
                        std::cout << ids[i];
                    }
                    std::cout << std::endl;
                }
                ++rendered;
                if (stopEarly) {
                    break;
                }
            } else {
                SDL_Delay(4);
            }
        }
        std::cout << "[demo] rendered " << rendered << "/" << args.frames
                  << " frames" << std::endl;
        if (args.untilLoaded > 0 && !settled) {
            std::cerr << "[demo] NOT settled after " << rendered << " frames"
                      << std::endl;
            exitCode = 1;
        }
        if (rendered == 0) {
            std::cerr << "[demo] no frame rendered" << std::endl;
            exitCode = 1;
        }
        if (tilesetLoaded) {
            std::cout << "[demo] tiles rendered (last frame): "
                      << tiles_renderer::Renderer::renderedTileCount()
                      << std::endl;
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
        if (args.printRss) {
            std::cout << "[rss] end=" << readRssKb() << "KB" << std::endl;
        }
        tiles_renderer::Renderer::shutdown();
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << "[demo] " << (exitCode == 0 ? "OK" : "FAILED") << std::endl;
    return exitCode;
}
