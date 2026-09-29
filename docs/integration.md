# Host Integration Guide

How to embed `tiles_renderer` in a real host app. The SDK is a static
library with **zero SDL dependency** (see `docs/adr/0003-sdk-no-sdl.md`):
the host owns the window and its lifecycle; the SDK only borrows the
native window handle for rendering.

All snippets below use the real public API from
`include/tiles_renderer/renderer.h` — nothing is invented. The Linux
snippet is compile-checked by `tests/host_integration_check.cpp`
(ctest `host_integration`).

## Lifecycle

```
initialize(config)          // once, on the render thread
  ├─ loadTileset(url)       // optional; replaces any previous tileset
  ├─ setOrbitCamera(...)    // optional; only affects a loaded tileset
  ├─ loop: renderFrame()    // every vsync
  ├─ resize(w, h)           // on host window/surface size change
  └─ shutdown()             // once, on the same thread
```

- `initialize` returns `false` for a null window / zero size, or when
  called twice without an intervening `shutdown()`.
- `loadTileset` accepts a local path, `file://` URL, or `http(s)://`
  URL. It blocks (bounded, ~30 s) until the root tile metadata arrives,
  so call it off the time-critical path or show a loading indicator.
  Loading a second tileset replaces the first; there is no separate
  unload — `shutdown()` drops everything.
- `renderFrame` returns `false` when the swap chain isn't ready yet
  (e.g. right after a resize); the host should just retry next frame.
- `readPixels` is for screenshots/debugging, not per-frame use.

## Threading model

**Every `Renderer` method except `version()` must be called on one
thread** — the render thread that called `initialize()`. There is no
internal locking on the API surface.

Internally the SDK spawns worker threads for tile I/O and content
parsing (cesium-native task system), but all Filament calls and all
public API state stay on the caller's thread: per-frame work is pumped
from inside `renderFrame()` via `dispatchMainThreadTasks()`. Do not
call `renderFrame()` (or any other method) from two threads
concurrently.

## Error handling

`initialize`, `loadTileset`, and `resize` return `bool`. On `false`,
`Renderer::lastError()` describes why (empty string on success):

```cpp
if (!tiles_renderer::Renderer::loadTileset(url)) {
    showToast("3D tiles failed: " + tiles_renderer::Renderer::lastError());
}
```

The message stays valid until the next SDK call.

## Camera

`setOrbitCamera(yawDegrees, pitchDegrees, distance)` orbits the
**tileset's local origin** (the P5 rebase point; ~(0,0,0) for small local
tilesets). There is no free `lookAt`/target API in this version — the
orbit target is deliberately fixed so tile *selection* (world space) and
tile *rendering* (rebased float32 space) stay consistent. A free camera
is future work.

## Resize semantics

The host resizes its own OS surface first, then calls
`Renderer::resize(w, h)`. The SDK recreates its swap chain for the same
native window handle and updates the viewport + camera aspect. Call it
between frames on the render thread. Same-size calls are a no-op.

---

## Windows (HWND + Vulkan)

```cpp
#include <windows.h>
#include "tiles_renderer/renderer.h"

// hwnd: your window; track WM_SIZE for w/h.
tiles_renderer::RendererConfig cfg;
cfg.window = static_cast<tiles_renderer::NativeWindowHandle>(hwnd);
cfg.width = width; cfg.height = height;
if (!tiles_renderer::Renderer::initialize(cfg)) return -1;

tiles_renderer::Renderer::setOrbitCamera(35.0f, 25.0f, 30.0f);
if (!tiles_renderer::Renderer::loadTileset("https://example.com/tileset.json"))
    log(tiles_renderer::Renderer::lastError());

MSG msg{};
while (running) {                       // your message pump
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_SIZE)     // host resized its surface first
            tiles_renderer::Renderer::resize(LOWORD(msg.lParam),
                                             HIWORD(msg.lParam));
        DispatchMessage(&msg);
    }
    tiles_renderer::Renderer::renderFrame();  // false => retry next frame
}
tiles_renderer::Renderer::shutdown();
```

## Android (ANativeWindow + Vulkan)

```cpp
#include <android/native_window.h>
#include "tiles_renderer/renderer.h"

// window: ANativeWindow* from the NativeActivity surface callbacks.
void onSurfaceCreated(ANativeWindow* window, int w, int h) {
    tiles_renderer::RendererConfig cfg;
    cfg.window = window;                // NativeWindowHandle IS ANativeWindow*
    cfg.width = w; cfg.height = h;
    tiles_renderer::Renderer::initialize(cfg);
    tiles_renderer::Renderer::loadTileset(assetPath("tileset.json"));
}
void onSurfaceChanged(ANativeWindow*, int w, int h) {
    tiles_renderer::Renderer::resize(w, h);   // surface already resized by OS
}
void onDrawFrame() {                            // GLSurfaceView.Renderer analog
    tiles_renderer::Renderer::renderFrame();    // same thread as initialize
}
void onSurfaceDestroyed() {
    tiles_renderer::Renderer::shutdown();       // same thread
}
```

## iOS (UIView + Metal)

```objc
// ViewController.mm — UIView* is passed as an opaque pointer.
#import "tiles_renderer/renderer.h"   // wrapped for ObjC++ as needed

- (void)viewDidLoad {
    CGSize s = self.view.bounds.size;
    tiles_renderer::RendererConfig cfg;
    cfg.window = static_cast<tiles_renderer::NativeWindowHandle>(
        (__bridge void*)self.view);   // UIView*
    cfg.width = s.width; cfg.height = s.height;
    tiles_renderer::Renderer::initialize(cfg);
    tiles_renderer::Renderer::loadTileset("path/to/tileset.json");
    self.link = [CADisplayLink displayLinkWithTarget:self
                                           selector:@selector(tick:)];
    [self.link addToRunLoop:[NSRunLoop mainRunLoop]
                    forMode:NSDefaultRunLoopMode];
}
- (void)viewDidLayoutSubviews {           // rotation / split-screen
    CGSize s = self.view.bounds.size;
    tiles_renderer::Renderer::resize(s.width, s.height);
}
- (void)tick:(CADisplayLink*)l {
    (void)l; tiles_renderer::Renderer::renderFrame();
}
- (void)dealloc { tiles_renderer::Renderer::shutdown(); }
```

## Web (canvas + WebGL) — stub, not yet rendering

> Honest status: the wasm build compiles the SDK with Filament **disabled**
> (the official Filament web release is `filament.js` only; there is no C++
> library to link — see `docs/adr/0006-platform-backends.md`). The snippet
> below shows the intended API shape once a WebGL C++ backend exists;
> today `initialize` validates the config, `renderFrame` is a no-op
> returning `true`, and `loadTileset` returns `false`.

```cpp
#include "tiles_renderer/renderer.h"
// WASM build: NativeWindowHandle is the canvas CSS selector string.
// The string must outlive the SDK (a literal is fine).

tiles_renderer::RendererConfig cfg;
cfg.window = "#canvas";
cfg.width = 1280; cfg.height = 720;
tiles_renderer::Renderer::initialize(cfg);   // stub: config check only
// loadTileset() returns false on wasm today (lastError explains why).
```

## Checklist for host developers

1. One thread for all SDK calls (except `version()`).
2. `renderFrame()` every frame; tolerate `false` (retry next frame).
3. Resize the OS surface first, then `resize()` — never the reverse.
4. Check `lastError()` whenever a `bool` API returns `false`.
5. Keep the WASM canvas-selector string alive for the SDK's lifetime.
6. `loadTileset` blocks up to ~30 s on first metadata — don't call it
   on the UI thread without a loading state.
