// Android on-device smoke harness (ADR-0023 G1).
//
// Built as libsmoke_jni.so by the repo's top-level CMake (android preset,
// real NDK toolchain only) and packaged into the samples/android-smoke APK
// as a prebuilt jniLib. Gradle does no NDK compile.
//
// The Java side (SmokeActivity) owns the SurfaceView and a status TextView.
// This file owns the render thread:
//
//   1. copy the p3 fixture out of APK assets into the app's private dir,
//   2. Renderer::initialize() on the ANativeWindow,
//   3. loadTileset() + orbit camera (same defaults as the desktop demo),
//   4. render until the tile pipeline settles — the same 20-frame streak
//      rule (tilesLoading == 0 && tilesLoaded > 0) as the desktop demo's
//      --until-loaded, capped at a 30s frame budget,
//   5. report PASS/FAIL on screen (big text for the user's screenshot) and
//      to <filesDir>/smoke_result.txt.
//
// Every Renderer call happens on this one worker thread (the SDK's
// threading contract: everything except version() on the thread that
// called initialize()).

#include <jni.h>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <tiles_renderer/renderer.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define SMOKE_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "TilesSmoke", __VA_ARGS__)

namespace {

JavaVM* g_vm = nullptr;
jobject g_activity = nullptr; // global ref, valid between start/stop
jmethodID g_onStatus = nullptr;
std::thread g_thread;
std::atomic<bool> g_stop{false};
ANativeWindow* g_window = nullptr;

// Must be callable from the worker thread.
void postStatus(const std::string& text) {
    if (g_vm == nullptr || g_activity == nullptr || g_onStatus == nullptr) {
        return;
    }
    JNIEnv* env = nullptr;
    const bool attached =
        g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK;
    if (attached && g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return;
    }
    jstring js = env->NewStringUTF(text.c_str());
    env->CallVoidMethod(g_activity, g_onStatus, js);
    env->DeleteLocalRef(js);
    if (attached) {
        g_vm->DetachCurrentThread();
    }
}

bool copyAsset(AAssetManager* mgr, const char* name,
               const std::filesystem::path& dst) {
    AAsset* a = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
    if (a == nullptr) {
        SMOKE_LOG("asset missing: %s", name);
        return false;
    }
    const off_t len = AAsset_getLength(a);
    std::vector<char> buf(static_cast<size_t>(len));
    const int got = AAsset_read(a, buf.data(), buf.size());
    AAsset_close(a);
    if (got != len) {
        SMOKE_LOG("short read: %s", name);
        return false;
    }
    std::ofstream out(dst, std::ios::binary);
    if (!out) {
        SMOKE_LOG("cannot write: %s", dst.c_str());
        return false;
    }
    out.write(buf.data(), buf.size());
    return static_cast<bool>(out);
}

// Same defaults as the desktop demo (samples/demo/main.cpp).
constexpr float kYawDeg = 30.0f;
constexpr float kPitchDeg = 18.0f;
constexpr float kDistance = 20.0f;

// Settle rule shared with the desktop demo's --until-loaded (P18/P30):
// tilesLoading == 0 && tilesLoaded > 0 for this many consecutive frames.
constexpr int kSettleStreak = 20;
// 30s frame budget at 60fps; the user is told to wait ~30s.
constexpr int kMaxFrames = 1800;

void renderThreadMain(std::string filesDir) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto elapsedMs = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   clock::now() - t0)
            .count();
    };

    std::string verdict = "FAIL";
    std::string detail = "not started";
    int frames = 0;
    long long tilesLoaded = -1;
    long long tilesFailed = -1;
    int renderedTiles = -1;
    long long settleMs = -1;

    const std::int32_t w = ANativeWindow_getWidth(g_window);
    const std::int32_t h = ANativeWindow_getHeight(g_window);
    SMOKE_LOG("surface %dx%d", w, h);

    tiles_renderer::RendererConfig cfg;
    cfg.window = g_window;
    cfg.width = static_cast<std::uint32_t>(w > 0 ? w : 0);
    cfg.height = static_cast<std::uint32_t>(h > 0 ? h : 0);
    if (!tiles_renderer::Renderer::initialize(cfg)) {
        detail = std::string("initialize failed: ") +
                 tiles_renderer::Renderer::lastError();
        SMOKE_LOG("%s", detail.c_str());
    } else {
        // P19: keep the tile cache small on a phone.
        tiles_renderer::Renderer::setMaxCachedBytes(64LL * 1024 * 1024);
        const std::string tileset =
            filesDir + "/p3_box_tileset/tileset.json";
        if (!tiles_renderer::Renderer::loadTileset(tileset)) {
            detail = std::string("loadTileset failed: ") +
                     tiles_renderer::Renderer::lastError();
            SMOKE_LOG("%s", detail.c_str());
        } else {
            tiles_renderer::Renderer::setOrbitCamera(kYawDeg, kPitchDeg,
                                                     kDistance);
            int streak = 0;
            bool settled = false;
            while (frames < kMaxFrames && !g_stop.load()) {
                if (!tiles_renderer::Renderer::renderFrame()) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(4));
                    continue;
                }
                ++frames;
                const auto st = tiles_renderer::Renderer::tileStats();
                tilesLoaded = st.tilesLoaded;
                tilesFailed = st.tilesFailed;
                if (st.tilesLoading == 0 && st.tilesLoaded > 0) {
                    if (++streak >= kSettleStreak) {
                        settled = true;
                        settleMs = elapsedMs();
                        break;
                    }
                } else {
                    streak = 0;
                }
            }
            renderedTiles = tiles_renderer::Renderer::renderedTileCount();
            if (g_stop.load()) {
                detail = "stopped by host";
            } else if (!settled) {
                detail = "tile pipeline did not settle within 30s";
            } else if (tilesFailed != 0) {
                detail = "tiles failed to load";
            } else if (renderedTiles <= 0) {
                detail = "no tiles rendered";
            } else {
                verdict = "PASS";
                detail = "ok";
            }
            SMOKE_LOG("verdict=%s detail=%s frames=%d loaded=%lld failed=%lld "
                      "rendered=%d settleMs=%lld",
                      verdict.c_str(), detail.c_str(), frames, tilesLoaded,
                      tilesFailed, renderedTiles, settleMs);
        }
    }

    char screen[512];
    std::snprintf(screen, sizeof(screen),
                  "%s\n"
                  "tiles: loaded=%lld failed=%lld rendered=%d\n"
                  "frames=%d settle=%lldms\n"
                  "%s",
                  verdict.c_str(), tilesLoaded, tilesFailed, renderedTiles,
                  frames, settleMs, detail.c_str());
    postStatus(screen);

    // Machine-readable copy for the report.
    const std::string resultPath = filesDir + "/smoke_result.txt";
    if (std::ofstream out(resultPath)) {
        out << "result=" << verdict << "\n"
            << "detail=" << detail << "\n"
            << "tilesLoaded=" << tilesLoaded << "\n"
            << "tilesFailed=" << tilesFailed << "\n"
            << "renderedTiles=" << renderedTiles << "\n"
            << "frames=" << frames << "\n"
            << "settleMs=" << settleMs << "\n"
            << "sdkVersion=" << tiles_renderer::Renderer::version() << "\n";
    }
    SMOKE_LOG("result written to %s", resultPath.c_str());
    // Leave the last presented frame on screen; the host tears down via
    // nativeStop().
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_com_tiles_smoke_SmokeActivity_nativeStart(JNIEnv* env, jobject thiz,
                                               jobject surface,
                                               jobject assetManager,
                                               jstring filesDir) {
    if (g_thread.joinable()) {
        SMOKE_LOG("already running");
        return;
    }
    env->GetJavaVM(&g_vm);
    g_activity = env->NewGlobalRef(thiz);
    jclass cls = env->GetObjectClass(thiz);
    g_onStatus = env->GetMethodID(cls, "onStatus", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(cls);

    const char* dirChars = env->GetStringUTFChars(filesDir, nullptr);
    const std::string dirStr(dirChars);
    env->ReleaseStringUTFChars(filesDir, dirChars);

    // Copy the fixture on the calling thread (16KB, instant); the render
    // thread then only touches the private dir.
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);
    const std::filesystem::path tileDir =
        std::filesystem::path(dirStr) / "p3_box_tileset";
    std::error_code ec;
    std::filesystem::create_directories(tileDir, ec);
    static const char* kFiles[] = {"tileset.json", "root.glb", "child_a.glb",
                                   "child_b.glb"};
    bool assetsOk = !ec;
    for (const char* f : kFiles) {
        const std::string asset = std::string("p3_box_tileset/") + f;
        if (!copyAsset(mgr, asset.c_str(), tileDir / f)) {
            assetsOk = false;
        }
    }
    if (!assetsOk) {
        postStatus("FAIL\ncould not stage fixture assets");
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        return;
    }

    g_window = ANativeWindow_fromSurface(env, surface);
    g_stop.store(false);
    postStatus("loading p3 fixture…");
    g_thread = std::thread(renderThreadMain, dirStr);
}

JNIEXPORT void JNICALL
Java_com_tiles_smoke_SmokeActivity_nativeStop(JNIEnv* env, jobject) {
    g_stop.store(true);
    if (g_thread.joinable()) {
        g_thread.join();
    }
    tiles_renderer::Renderer::shutdown();
    if (g_window != nullptr) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
    if (g_activity != nullptr) {
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
    }
    g_onStatus = nullptr;
    SMOKE_LOG("stopped");
}

} // extern "C"
