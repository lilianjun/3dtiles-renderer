# Filament v1.77.0 — API Reference

> **Scope:** Public C++ API of the Filament real-time rendering engine, version v1.77.0,
> as shipped in the prebuilt binary distribution (headers under `include/`).
> Namespaces: `filament::` (core), `filament::math`, `filament::backend`,
> `filament::gltfio`, `filament::filamat`, `utils`.
> Every substantive claim cites a source: a header path relative to the
> distribution root (e.g. `include/filament/Engine.h`), a full official docs URL,
> or a sample path in https://github.com/google/filament.

---

## 1. Overview

Filament is a real-time physically-based rendering (PBR) engine using a
**clustered forward renderer** with Cook–Torrance microfacet specular BRDF,
Lambertian diffuse, HDR/linear lighting, and physical light units
(source: [Filament README](https://github.com/google/filament/blob/main/README.md)).

**Supported backends** (selectable at `Engine` creation; see §3):
OpenGL 4.1+ (Linux/macOS/Windows), OpenGL ES 3.0+ (Android/iOS),
Metal (macOS/iOS), Vulkan 1.0 (Android/Linux/macOS/Windows),
WebGPU (Android/Linux/macOS/Windows)
(source: [Filament README](https://github.com/google/filament/blob/main/README.md)).

Key public namespaces:

| Namespace | Contents |
|---|---|
| `filament::` | Core: `Engine`, `Renderer`, `Scene`, `View`, `Camera`, `SwapChain`, `Fence`, `Material`, `Texture`, buffers, managers |
| `filament::math` | Vector/matrix/quaternion math types |
| `filament::backend` | Driver enums (`Backend`, `ElementType`, `PrimitiveType`, `TextureFormat`, `PixelDataFormat`), `BufferDescriptor`, `PixelBufferDescriptor` |
| `filament::filamat` | `MaterialBuilder` — runtime material compiler (also powers the `matc` CLI tool) |
| `filament::gltfio` | glTF 2.0 loading: `AssetLoader`, `ResourceLoader`, `MaterialProvider`, `FilamentAsset`, `FilamentInstance`, `Animator` |
| `utils` | `Entity`, `EntityManager`, containers, logging |

Most core classes inherit from `filament::FilamentAPI`
(`include/filament/FilamentAPI.h`), which **forbids heap allocation**
(`operator new` is deleted), **forbids stack allocation** (protected ctor),
and forbids copy/move. Objects can only be created via builders/factories
(`Engine::create*`, `Xxx::Builder::build`) and destroyed via
`Engine::destroy()`.

---

## 2. Architecture: Engine, SwapChain, Renderer, Scene, View, Camera, Fence

### 2.1 `Engine` — main entry point (`include/filament/Engine.h`)

The `Engine` is the factory and bookkeeper for every Filament object: it tracks
all created resources, manages the rendering thread and a pool of worker
threads. There is one `Engine` per hardware context
(e.g. one per OpenGL ES context).

Creation (two equivalent routes):

```cpp
#include <filament/Engine.h>
using namespace filament;

Engine* engine = Engine::create();            // default backend, or
Engine* engine = Engine::Builder()
        .backend(Engine::Backend::VULKAN)
        .build();
```

`Engine::create()` is a backward-compatibility helper
(`Engine::create(Backend backend = Backend::DEFAULT)`); `Engine::Builder`
additionally offers `.platform(...)`, `.config(...)`,
`.featureLevel(...)`, `.stereoscopicOptions(...)`,
`.asyncCommandQueueSize(...)`, `.sharedUboMaxAge(...)`, `.workaround(...)`,
`.jobSystem(...)` etc.

`Engine::Config` customizes memory footprint (defaults in
`include/filament/Engine.h`):

| Field | Default | Meaning |
|---|---|---|
| `commandBufferSizeMB` | 3 | Low-level command buffer arena; too small ⇒ abort/render errors |
| `perRenderPassArenaSizeMB` | 3 | Per-frame high-level command arena (froxel data, etc.) |
| `perFrameCommandsSizeMB` | 2 | Per-frame high-level command buffer; relates to draw calls/frame |
| `driverHandleArenaSizeMB` | 0 | Backend handle arena (0 = platform default) |
| `jobSystemThreadCount` | 0 | Worker threads (0 = heuristic); `SINGLE_THREADED` = force single-threaded |
| `metalUploadBufferSizeBytes` | 512 KiB | Metal shared staging buffer (Metal only) |
| `materialCacheCapacity` / `programCacheCapacity` | 0 | LRU caches for material definitions/program specializations (0 = destroy immediately) |
| `enableMultipleDirectionalLights` | false | Allow up to 4 extra directional lights (no shadows, no sun disk) |

Notable `Engine` methods:

- Factories: `createSwapChain(void* nativeWindow, flags)`,
  `createSwapChain(width, height, flags)` (headless),
  `createRenderer()`, `createView()`, `createScene()`,
  `createCamera(utils::Entity entity)`, `createFence()`, `createSync()`.
- Destruction: overloaded `bool destroy(const X* p)` for every resource type
  (returns false on failure); `void destroy(utils::Entity e)` strips all
  Filament components from an entity. `Engine::destroy(&engine)` / `Engine::destroy(engine)`
  destroys the engine last (static, thread-safe per doc comments).
- Validity/debug: `isValid(const X* p)` per type; `isValid(const Material*, const MaterialInstance*)`;
  `getXxxCount()` debug counters (buffers, views, scenes, textures, …).
- Managers: `getRenderableManager()`, `getLightManager()`,
  `getTransformManager()`, `getEntityManager()`; `getBackend()`,
  `getFeatureLevel()`, `getJobSystem()`, `getDriver()`,
  `getDefaultMaterial()`, `getDebugRegistry()`.
- Feature levels: `backend::FeatureLevel` = `FEATURE_LEVEL_0` (OpenGL ES 2.0
  features) … `FEATURE_LEVEL_3` (ES 3.1 + 31 texture units + cubemap arrays);
  default is `FEATURE_LEVEL_1` (ES 3.0 features)
  (`include/backend/DriverEnums.h`).

Typical render loop (from the doc comment in `include/filament/Engine.h`):

```cpp
Engine* engine       = Engine::create();
SwapChain* swapChain = engine->createSwapChain(nativeWindow);
Renderer* renderer   = engine->createRenderer();
Scene* scene         = engine->createScene();
View* view           = engine->createView();
view->setScene(scene);

do {
    // typically we wait for VSYNC and user input events
    if (renderer->beginFrame(swapChain)) {
        renderer->render(view);
        renderer->endFrame();
    }
} while (!quit);

engine->destroy(view);
engine->destroy(scene);
engine->destroy(renderer);
engine->destroy(swapChain);
Engine::destroy(&engine);   // clears engine*
```

Also documented in the project README's "Rendering with Filament" section:
[Filament README](https://github.com/google/filament/blob/main/README.md).

### 2.2 `SwapChain` — native renderable surface (`include/filament/SwapChain.h`)

Represents an OS *native* renderable surface (window/view), handed to Filament
as a platform-appropriate `void*`. Created by `Engine::createSwapChain`.
Flags: `CONFIG_TRANSPARENT`, `CONFIG_READABLE`, `CONFIG_ENABLE_XCB` (Linux),
`CONFIG_APPLE_CVPIXELBUFFER`, `CONFIG_SRGB_COLORSPACE`, `CONFIG_HAS_STENCIL_BUFFER`,
`CONFIG_PROTECTED_CONTENT`. Query helpers:
`isSRGBSwapChainSupported(engine)`, `isMSAASwapChainSupported(engine, samples)`,
`isProtectedContentSupported(engine)`, `getNativeWindow()`,
`setFrameRate(...)` (with `isFrameRateChangeSupported()`),
`setFrameScheduledCallback(handler, callback, user)` /
`isFrameScheduledCallbackSet()`.

### 2.3 `Renderer` — frame submission (`include/filament/Renderer.h`)

Manages the frame protocol. One `Renderer` per `Engine` is typical; multiple
views may be rendered per frame.

- `bool beginFrame(SwapChain* swapChain, uint64_t vsyncSteadyClockTimeNano = 0,
  backend::FrameScheduledCallback callback = {}, void* user = nullptr)`.
  Returns `false` to signal the frame should be skipped (too much GPU work
  queued, or a backend exception was delivered to the main thread).
  When `true`, `render()` + `endFrame()` are mandatory.
- `void render(View const* view)` — must be called **after** `beginFrame()`
  and **before** `endFrame()`, from the Engine's main thread (or with external
  synchronization across `Renderer` instances).
- `void endFrame()`.
- `void renderStandaloneView(View const* view)` — render into a `RenderTarget`
  without `beginFrame()`/`endFrame()`; must be called from the Engine's main
  thread.
- Presentation pacing: `setDisplayInfo(DisplayInfo{refreshRate})`,
  `setFrameRateOptions(FrameRateOptions)`, `setVsyncTime(ns)`,
  `skipFrame(...)`, `shouldRenderFrame()`, `setPresentationTime`,
  `setDesiredPresentationTime`, `setRenderingDeadline`.
- `setClearOptions(ClearOptions{clearColor, clear, discard})` /
  `getClearOptions()` — the clear happens outside tone mapping. It is
  recommended to use a `Skybox` to clear opaque views, or black/transparent
  clear color.
- Readback: `readPixels(xoffset, yoffset, width, height, PixelBufferDescriptor&&)`
  (swap-chain or `RenderTarget` variants), completed callback on the main
  thread. `copyFrame(dstSwapChain, dstViewport, srcViewport, flags)`.
- `getFrameInfoHistory(n)` / `getMaxFrameHistorySize()` for GPU timing
  (`FrameInfo` with `beginFrame`, `endFrame`, `gpuFrameDuration`, `vsync`, …).

### 2.4 `Scene` — container of entities (`include/filament/Scene.h`)

Holds the entities that make up the world.

- `addEntity(entity)` / `addEntities(entities, count)` /
  `remove(entity)` / `removeEntities(...)` / `removeAllEntities()`.
- Environment: `setSkybox(Skybox*)`, `getSkybox()`,
  `setIndirectLight(IndirectLight*)`, `getIndirectLight()`.
- Introspection: `getEntityCount()`, `getRenderableCount()`,
  `getLightCount()`, `hasEntity(entity)`, `forEach(functor)`.

### 2.5 `View` — how a Scene is rendered (`include/filament/View.h`)

Binds a `Scene`, a `Camera`, a viewport and a render target, and configures
all per-view rendering features.

- `setScene(Scene*)` / `getScene()`; `setCamera(Camera*)` / `getCamera()` /
  `hasCamera()`; `setViewport(Viewport{left, bottom, width, height})`;
  `setRenderTarget(RenderTarget*)` (default = the swap chain).
- Per-view feature toggles/options (set/get pairs): `setShadowingEnabled`,
  `setScreenSpaceRefractionEnabled`, `setPostProcessingEnabled` (default true),
  `setAntiAliasing(AntiAliasing)` (`NONE`/`FXAA`), `setSampleCount` (MSAA),
  `setTemporalAntiAliasingOptions`, `setAmbientOcclusionOptions` (SSAO),
  `setBloomOptions`, `setFogOptions`, `setDepthOfFieldOptions`,
  `setVignetteOptions`, `setDithering`, `setColorGrading(ColorGrading*)`,
  `setDynamicResolutionOptions`, `setMultiSampleAntiAliasingOptions`,
  `setScreenSpaceReflectionsOptions`, `setGuardBandOptions`,
  `setStereoscopicOptions`, `setFrustumCullingEnabled`,
  `setBlendMode` (`OPAQUE`/`TRANSLUCENT`), `setVisibleLayers(select, values)`
  (+ `setLayerEnabled(layer, enabled)` helper), `setShadowType`,
  `setDynamicLightingOptions(zLightNear, zLightFar)`,
  `setRenderQuality`, `setDebugCamera`, `setFrontFaceWindingInverted`,
  `setStencilBufferEnabled`.
- Picking: `pick(x, y, handler, PickingQuery::Callback, user)` with
  `PickingQueryResult` (renderable entity, depth, frag coords).
- Froxel viz: `setFroxelVizEnabled`, `getFroxelConfigurationInfo`.

### 2.6 `Camera` — eye through which the scene is viewed (`include/filament/Camera.h`)

A `Camera` is a component attached to an entity:
`engine->createCamera(entity)`; destroyed via
`engine->destroyCameraComponent(entity)`.

- `setProjection(Projection, left, right, bottom, top, near, far)` —
  `Projection::{PERSPECTIVE, ORTHO}`; or
  `setProjection(fovInDegrees, aspect, near, far, direction = Fov::VERTICAL)`.
- `setLensProjection(focalLengthInMillimeters, aspect, near, far)`,
  `setCustomProjection(projection, near, far)` (optionally a separate
  `projectionForCulling`), `setCustomEyeProjection(...)` (stereoscopic),
  `setScaling(double2)`, `setShift(double2)` (tilt/shift).
- `setModelMatrix(const math::mat4&)` (double precision) or
  `lookAt(eye, center, up)` (double3); `setEyeModelMatrix(eyeId, model)`.
- Queries: `getProjectionMatrix()`, `getViewMatrix()`,
  `getModelMatrix()`, `getCullingProjectionMatrix()`, `getNear()`,
  `getCullingFar()`, `getFrustum()`, `getFieldOfViewInDegrees(Fov)`,
  `getEntity()`, `getAperture()`, `getShutterSpeed()`, `getSensitivity()`,
  exposure helpers (`getExposure()`), `setExposure(...)`.

### 2.7 `Fence` / `Sync` — GPU/CPU synchronization

- `Fence` (`include/filament/Fence.h`): "used to synchronize the application
  main thread with filament's rendering thread." `Fence* f = engine->createFence()`;
  `f->wait(Mode mode = FLUSH, timeout = FENCE_WAIT_FOR_EVER)` returns
  `FenceStatus` (`ERROR`, `CONDITION_SATISFIED`, `TIMEOUT_EXPIRED`); static
  `waitAndDestroy(Fence*, Mode)`. Do not call `wait(..., DONT_FLUSH)` from the
  same thread that would execute the fence's work (deadlock risk).
- `Sync` (`include/filament/Sync.h`, new in recent versions): lower-level;
  `engine->createSync()`; `getExternalHandle(handler, callback, userData)`
  exports a platform sync handle (e.g. for external compositors).

---

## 3. Core rendering path: entities, components, buffers

Filament uses an entity–component system (`utils::Entity`, §7).
`RenderableManager`, `LightManager` and `TransformManager` are component
managers obtained from the `Engine`:

```cpp
RenderableManager& rcm = engine->getRenderableManager();
LightManager&      lm  = engine->getLightManager();
TransformManager&  tm  = engine->getTransformManager();
```

### 3.1 `VertexBuffer::Builder` (`include/filament/VertexBuffer.h`)

```cpp
#include <filament/VertexBuffer.h>
#include <backend/BufferDescriptor.h>

struct V { filament::math::float3 pos; filament::math::float2 uv; };
static V verts[...];

VertexBuffer* vb = VertexBuffer::Builder()
        .vertexCount(N)
        .bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3)
        .attribute(VertexAttribute::UV0,      0, VertexBuffer::AttributeType::FLOAT2,
                   sizeof(filament::math::float3))   // byteOffset for interleaved data
        .build(*engine);

vb->setBufferAt(*engine, 0, backend::BufferDescriptor(
        verts, sizeof(verts), nullptr));   // copy-upload; callback optional
```

- `bufferCount(uint8_t)` is **mandatory** (max 8); `vertexCount(uint32_t)`
  sets vertices per buffer.
- `attribute(attribute, bufferIndex, attributeType, byteOffset = 0, byteStride = 0)`;
  stride 0 ⇒ attribute size is used (tight packing).
- `normalized(attribute, true)` maps integer attributes to [0,1] in the shader
  (integer types only).
- `enableBufferObjects(true)` switches to shared `BufferObject` mode — then use
  `setBufferObjectAt()` instead of `setBufferAt()`.
- `advancedSkinning(true)` for GPU skinning with bone indices/weights in the
  vertex buffer.
- `VertexBuffer::AttributeType` = `backend::ElementType`: `BYTE…HALF4`,
  `FLOAT`, `FLOAT2`, `FLOAT3`, `FLOAT4`, `UBYTE4`, `USHORT4`, …
  (`include/backend/DriverEnums.h`).
- `setBufferAt(engine, bufferIndex, BufferDescriptor&&, byteOffset = 0)` —
  `byteOffset` must be a multiple of 4. `isCreationComplete()`.
- The builder has `async(handler, callback, user)` for asynchronous creation;
  this requires the Engine's asynchronous mode (see §9).

Vertex attribute slots (`include/filament/MaterialEnums.h`):

```
POSITION=0  (float3 XYZ)
TANGENTS=1  (tangent/bitangent/normal packed as a QUATERNION, float4)
COLOR=2     (float4)
UV0=3, UV1=4 (float2)
BONE_INDICES=5 (uvec4), BONE_WEIGHTS=6 (normalized float4)
CUSTOM0..CUSTOM7 = 8..15 (also used as MORPH_POSITION_0..3 / MORPH_TANGENTS_0..3 aliases)
```

### 3.2 `IndexBuffer::Builder` (`include/filament/IndexBuffer.h`)

```cpp
#include <filament/IndexBuffer.h>

IndexBuffer* ib = IndexBuffer::Builder()
        .indexCount(indexCount)
        .bufferType(IndexBuffer::IndexType::USHORT)   // or UINT
        .build(*engine);

ib->setBuffer(*engine, backend::BufferDescriptor(
        indices, indexCount * sizeof(uint16_t), nullptr));
```

`IndexType::{USHORT, UINT}` (16/32-bit). A single `IndexBuffer` may be shared
by several renderables. `getIndexCount()`.

### 3.3 `RenderableManager::Builder` (`include/filament/RenderableManager.h`)

A complete renderable ties geometry to a material instance
(doc-comment example in the header; also the project README's
"Rendering with Filament" section):

```cpp
#include <filament/RenderableManager.h>

utils::Entity e = utils::EntityManager::get().create();
RenderableManager::Builder(1)                       // 1 primitive
        .boundingBox({{ -1, -1, -1 }, { 1, 1, 1 }}) // filament::Box (min, max)
        .material(0, materialInstance)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                 vertexBuffer, indexBuffer, 0, indexCount)
        .culling(false)
        .receiveShadows(false)
        .castShadows(false)
        .build(*engine, e);
scene->addEntity(e);
```

- `Builder(size_t count)` = number of primitives; each primitive needs a
  paired `geometry(...)` + `material(...)` call.
- `geometry(index, PrimitiveType type, VertexBuffer*, IndexBuffer*, offset, count)`
  (offset/count in indices; minIndex/maxIndex variants exist).
- `PrimitiveType` = `backend::PrimitiveType`: `POINTS`, `LINES`, `LINE_STRIP`,
  `TRIANGLES`, `TRIANGLE_STRIP` (values match GL)
  (`include/backend/DriverEnums.h`).
- Common options: `boundingBox(Box)`, `culling(bool)`,
  `castShadows(bool)`, `receiveShadows(bool)`,
  `screenSpaceContactShadows(bool)`, `fog(bool)`,
  `layerMask(select, values)`, `priority(uint8_t)` (0..7),
  `channel(uint8_t)`, `lightChannel(channel, enable)`,
  `blendOrder(primitiveIndex, order)` / `globalBlendOrderEnabled(...)`,
  `geometryType(DYNAMIC / STATIC_BOUNDS / STATIC)`,
  `skinning(...)`, `morphing(...)`, `instances(count)` (GPU instancing),
  `enableSkinningBuffers(bool)`.
- `build(Engine&, utils::Entity)` returns `Result::{Success = 0, Error = -1}`.
- Runtime updates: `setGeometryAt`, `setMaterialInstanceAt`,
  `setBoundingBox`, `setLayerMask`, `setPriority`, `setCulling`,
  `setCastShadows`, `setReceiveShadows`, `setScreenSpaceContactShadows`,
  `setBlendOrderAt`, `setGlobalBlendOrderEnabledAt`,
  `setSkinningBuffer`, `setMorphTargetBufferAt`, `setBones`…

### 3.4 `TransformManager` (`include/filament/TransformManager.h`)

Scene-graph transforms (parent/child hierarchy, local → world):

```cpp
TransformManager& tm = engine->getTransformManager();
auto inst = tm.getInstance(entity);                    // Instance handle
tm.create(entity, parentInstance /* or {} */, localTransform /* math::mat4f */);
tm.setTransform(inst, math::mat4f{...});               // also math::mat4 overload
math::mat4f world = tm.getWorldTransform(inst);
tm.setParent(inst, newParent);
tm.getChildren(inst, children, count);                 // or getChildrenRange()
tm.openLocalTransformTransaction();                    // batch many updates…
tm.commitLocalTransformTransaction();                  // …much faster for deep trees
tm.destroy(entity);
```

`setAccurateTranslationsEnabled(true)` enables double-precision translation
handling (`getTransformAccurate()`).

### 3.5 `LightManager::Builder` (`include/filament/LightManager.h`)

```cpp
#include <filament/LightManager.h>

utils::Entity light = utils::EntityManager::get().create();
LightManager::Builder(LightManager::Type::SUN)
        .direction({ 0.0f, -1.0f, -0.2f })
        .color({ 1.0f, 0.95f, 0.9f })
        .intensity(110000.0f)          // lux for directional; see below
        .castShadows(true)
        .shadowOptions(LightManager::ShadowOptions{
                .mapSize = 2048, .shadowCascades = 4 })
        .build(*engine, light);
scene->addEntity(light);
```

- `Type::{SUN, DIRECTIONAL, POINT, FOCUSED_SPOT, SPOT}`.
  `SUN` also draws a sun disk; `FOCUSED_SPOT` is physically correct,
  `SPOT` disables outer-cone/illumination coupling.
- Physical units: `intensity(float)` = luminous power in **lumens** for point/spot,
  **lux** for directional; `intensity(float watts, float efficiency)`;
  `intensityCandela(float)` for candela. `color(LinearColor)` (linear RGB;
  physical units multiply).
- `position(float3)`, `direction(float3)`, `falloff(float radius)` (point/spot),
  `spotLightCone(inner, outer)` (radians), `sunAngularRadius(degrees)`,
  `sunHaloSize` / `sunHaloFalloff`, `castShadows(bool)`,
  `castLight(bool)`, `lightChannel(channel, enable)`.
- `ShadowOptions`: `mapSize` (power of two, ≥ 8), `shadowCascades` (≤ 4),
  `cascadeSplitPositions`, `constantBias`, `normalBias`, `shadowFar`,
  `shadowNearHint`, `shadowFarHint`, `stable`, `lispsm`, `polygonOffset…`,
  `elided`, `vsm`…; static helpers `computeUniformSplits`,
  `computeLogSplits`, `computePracticalSplits`.
- By default only the dominant directional light is evaluated; set
  `Engine::Config::enableMultipleDirectionalLights = true` for up to 4 more.

---

## 4. Material system

### 4.1 Concepts (official docs)

From the [Filament Materials Guide](https://google.github.io/filament/Materials.md.html):

- **Material definition** — a text file (`.mat`) that describes everything a
  material needs: material model, named user-controllable parameters, raster
  state, and **vertex + fragment shader code** (GLSL).
- **Material package** — the compiled binary (conventionally `.filamat`)
  produced from a definition by the **`matc`** tool. It contains all shader
  variants for the target platforms/APIs.
- **Material instance** — a runtime reference to a material plus concrete
  values for its parameters. Created/manipulated from code.

Material models: `lit` (standard PBR), `subsurface`, `cloth`, `unlit`,
legacy `specularGlossiness`. Standard-model properties include `baseColor`,
`roughness`, `metallic`, `reflectance`, `normal`, `emissive`,
`ambientOcclusion`, `clearCoat`, `anisotropy`, `sheenColor`, `transmission`,
`ior`, `thickness`, `dispersion`, `shadowStrength`, … (full tables in the guide).

Minimal `.mat` example (structure per the Materials Guide; parameter types
declared under `material { parameters : [ … ] }`, shader entry points
`void material(inout MaterialInputs material)` in `fragment { }` / optional
`vertex { }`):

```text
material {
    name : LitColor,
    shadingModel : lit,
    parameters : [
        { type : float3, name : baseColor }
    ]
}
fragment {
    void material(inout MaterialInputs material) {
        prepareMaterial(material);
        material.baseColor.rgb = materialParams.baseColor;
        material.roughness = 0.45;
        material.metallic  = 0.0;
    }
}
```

### 4.2 `matc` — material compiler

Ships in the release archives' `bin/` directory (same directory as `cmgen`,
`gltf_viewer`, …). Simplest usage
(source: [Materials.md.html — "Compiling materials"](https://google.github.io/filament/Materials.md.html)):

```text
$ matc -o ./materials/bin/car_paint.filamat ./materials/src/car_paint.mat
```

Relevant flags (`matcFlags` table in the guide):

| Flag | Value | Usage |
|---|---|---|
| `-o`, `--output` | path | Output file path |
| `-p`, `--platform` | desktop / mobile / all | Target platform(s) |
| `-a`, `--api` | opengl / vulkan / all | Target graphics API |
| `-S`, `--optimize-size` | — | Optimize for size instead of performance |
| `-r`, `--reflect` | parameters | Emit the specified metadata as JSON |

`matc` validates shaders and reports errors with line numbers of the `.mat`
source. Runtime compilation is also possible via `filamat::MaterialBuilder`
(`include/filamat/MaterialBuilder.h`): `MaterialBuilder::init()`,
configure `.name()/.shading()/.require()/.material(glsl)`,
`targetApi()`, `platform()`, `.build()` → `filamat::Package`
(`pkg.getData()`, `pkg.getSize()`), then `MaterialBuilder::shutdown()`.
The [iOS sample guide](https://github.com/google/filament/blob/HEAD/docs_src/src_mdbook/src/samples/ios.md)
demonstrates this, but notes `matc` at build time is recommended over
runtime compilation.

### 4.3 `Material::Builder` (`include/filament/Material.h`)

```cpp
#include <filament/Material.h>

// package = binary blob from matc / filamat (must stay valid until build() returns)
Material* mat = Material::Builder()
        .package(payload, payloadSize)
        .build(*engine);          // returns nullptr on failure — check it
if (!mat) { /* handle error */ }
```

- `package(const void* payload, size_t size)` — "Pointer to the material data,
  must stay valid until `build()` is called."
- `constant(name, value)` — bake a specialization constant into the material.
- `sphericalHarmonicsBandCount(n)`, `shadowSamplingQuality(HARD/LOW)`,
  `uboBatching(UboBatchingMode::DEFAULT/DISABLED)`.
- `compile(CompilerPriorityQueue, variants, handler, callback, user)` —
  pre-compile shader variants asynchronously; callback runs on the main thread.
- Introspection: `getName()`, `getShading()`, `getInterpolation()`,
  `getBlendingMode()`, `getVertexDomain()`, `getCullingMode()`,
  `getTransparencyMode()`, `isColorWriteEnabled()`, `isDepthWriteEnabled()`,
  `isDepthCullingEnabled()`, `isDoubleSided()`, `isAlphaToCoverageEnabled()`,
  `getMaskThreshold()`, `hasShadowMultiplier()`, `getRefractionMode()`,
  `getRefractionType()`, `getReflectionMode()`,
  `getRequiredAttributes()` (bitset of `VertexAttribute`s the shader needs),
  `getFeatureLevel()`, `getSupportedVariants()`,
  `getParameterCount()`, `getParameters(ParameterInfo*, count)`,
  `hasParameter(name)`, `isSampler(name)`,
  `getParameterTransformName(samplerName)`, `getSource()` (.mat source view).
- `ParameterInfo` = `{ name, isSampler, isSubpass, type|samplerType|subpassType,
  count, precision }`.

Instances:

```cpp
MaterialInstance* mi = mat->createInstance("myInstance"); // or:
MaterialInstance* d  = mat->getDefaultInstance();        // shared default instance
```

`setDefaultParameter(name, value)` helpers on `Material` forward to the
default instance (scalar, texture+sampler, rgb/rgba color variants).

### 4.4 `MaterialInstance` (`include/filament/MaterialInstance.h`)

Parameter setting (templates; supported scalar types include `bool`,
`int32_t`, `float`, `math::float2/3/4`, `math::int2/3/4`, `math::mat3f/4f`, …):

```cpp
mi->setParameter("roughness", 0.4f);
mi->setParameter("baseColor", filament::RgbType::LINEAR, filament::math::float3{1,0,0});
mi->setParameter("albedoMap", texture, filament::TextureSampler{
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT });
mi->setParameter("weights", values, count);      // array form
float r = mi->getParameter<float>("roughness");
```

- `setParameter(name, RgbType::LINEAR|SRGB, float3)` /
  `setParameter(name, RgbaType::LINEAR|SRGB, float4)` — color-space-aware
  color parameters.
- `setConstant` / `getConstant` — specialization constants
  (`int32_t`/`float`/`bool`).
- `duplicate(other, name = nullptr)` — clone an instance (same material).
- Per-instance raster state: `setScissor(l,b,w,h)` / `unsetScissor()`,
  `setPolygonOffset(scale, constant)`, `setMaskThreshold(threshold)`.
- `compile(priority, variants, handler, callback, user)` — pre-compile for
  this instance.
- **Lifecycle:** all `MaterialInstance`s must be destroyed (`engine->destroy`)
  **before** their `Material` (documented `@attention` on
  `Engine::destroy(const Material*)`).

---

## 5. Texture system

### 5.1 `Texture::Builder` (`include/filament/Texture.h`)

```cpp
#include <filament/Texture.h>

Texture* tex = Texture::Builder()
        .width(w).height(h)
        .levels(0xff)                                   // 0xff = generate all mip levels count
        .sampler(Texture::Sampler::SAMPLER_2D)
        .format(Texture::InternalFormat::RGBA8)
        .usage(Texture::Usage::DEFAULT)
        .build(*engine);

tex->setImage(*engine, 0, Texture::PixelBufferDescriptor(
        pixels, byteSize,
        Texture::Format::RGBA, Texture::Type::UBYTE));
tex->generateMipmaps(*engine);
```

Type aliases on `Texture`: `Sampler = backend::SamplerType`,
`InternalFormat = backend::TextureFormat`, `Format = backend::PixelDataFormat`,
`Type = backend::PixelDataType`, `Usage = backend::TextureUsage`,
`Swizzle = backend::TextureSwizzle`,
`PixelBufferDescriptor = backend::PixelBufferDescriptor`.

- `Sampler` (`include/backend/DriverEnums.h`): `SAMPLER_2D`,
  `SAMPLER_2D_ARRAY`, `SAMPLER_CUBEMAP`, `SAMPLER_EXTERNAL` (e.g. Android
  external video textures), `SAMPLER_3D`, `SAMPLER_CUBEMAP_ARRAY`
  (requires feature level 2).
- `InternalFormat` (`backend::TextureFormat`): large enum covering
  sized/unsized color (`RGBA8`, `SRGB8_A8`, `R11F_G11F_B10F`, …),
  depth/stencil (`DEPTH24`, `DEPTH24_STENCIL8`, …), compressed
  (`ETC2_EAC_RGBA8`, …). A representative capabilities table lives in the
  doc comment in `include/backend/DriverEnums.h`.
- `Usage` (`backend::TextureUsage` bitmask): `DEFAULT`, `COLOR_ATTACHMENT`,
  `DEPTH_ATTACHMENT`, `STENCIL_ATTACHMENT`, `UPLOADABLE`, `SAMPLEABLE`,
  `SUBPASS_INPUT`, `BLIT_SRC`, `BLIT_DST`, `PROTECTED`, `SHAREABLE`…
- Query helpers (static): `isTextureFormatSupported(engine, format)`,
  `isTextureFormatMipmappable(engine, format)`, `isTextureFormatCompressed(format)`,
  `isTextureSwizzleSupported(engine)`, `isProtectedTexturesSupported(engine)`,
  `getMaxTextureSize(engine, Sampler)`, `getMaxArrayTextureLayers(engine)`,
  `computeTextureDataSize(Format, Type, stride, height, alignment)`,
  `validatePixelFormatAndType(internalFormat, format, type)`.
- `swizzle(r, g, b, a)` remaps channels (only if swizzle supported).
- `external()` marks an external texture (implies `SAMPLER_EXTERNAL`).
- `import(intptr_t id)` — wrap a platform texture id.
- Instance API: `getWidth(level = 0)` / `getHeight` / `getDepth` /
  `getLevels()` / `getTarget()` / `getFormat()`; `setImage(engine, level,
  xoffset, yoffset, zoffset, width, height, depth, PixelBufferDescriptor&&)`
  (2D convenience overloads exist); `setImageAsync(...)`;
  `setExternalImage(engine, image)` (+ planar variant with `plane`);
  `setExternalStream(engine, Stream*)`; `generateMipmaps(engine)`;
  `isCreationComplete()`.

### 5.2 `PixelBufferDescriptor` (`include/backend/PixelBufferDescriptor.h`)

"A descriptor to an image in main memory, typically used to transfer image
data from the CPU to the GPU. A PixelBufferDescriptor **owns the memory buffer
it references, therefore it cannot be copied, but can be moved**." The
buffer is released via the user callback (default: `free`) when the
descriptor is destroyed. The callback is invoked **on the main Filament
thread**, must be lightweight, and **must not call Filament APIs**
(`include/backend/BufferDescriptor.h`).

```cpp
using PBD = filament::Texture::PixelBufferDescriptor;
tex->setImage(*engine, 0, PBD(pixels, byteSize,
        PBD::PixelDataFormat::RGBA, PBD::PixelDataType::UBYTE,
        [](void* buf, size_t, void*) { delete[] (uint8_t*)buf; }));
```

Constructors cover: raw `(buffer, size, format, type, alignment, left, top,
stride, handler, callback, user)`, simplified variants, and compressed
`(buffer, size, CompressedPixelDataType, imageSize, …)`. Static
`make<T, &T::method>(…)` helpers bind a member function as the release
callback.

### 5.3 `TextureSampler` (`include/filament/TextureSampler.h`)

Value type describing how a texture is sampled in a material:

```cpp
filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
sampler.setAnisotropy(8.0f);
sampler.setCompareMode(filament::TextureSampler::CompareMode::COMPARE_TO_TEXTURE,
                       filament::TextureSampler::CompareFunc::LE);  // shadow maps
```

Setters: `setMinFilter`, `setMagFilter`, `setWrapModeS/T/R`, `setAnisotropy`,
`setCompareMode`. Enums come from `backend::SamplerDescriptor`
(`MinFilter`, `MagFilter`, `WrapMode::{CLAMP_TO_EDGE, REPEAT, MIRRORED_REPEAT}`,
`CompareMode`, `CompareFunc`) — `include/backend/DriverEnums.h`.

---

## 6. Math library (`include/math/`)

`filament::math` types (`include/math/mathfwd.h`):

```cpp
#include <math/vec3.h>
#include <math/mat4.h>
using namespace filament::math;

float3  p{1,2,3};   float4 c{1,1,1,1};   double3 d;
mat4f   m = mat4f{};  mat4 view;         // TMat44<double>
quatf   q;            half h(0.5f);
```

Aliases: `float2/3/4`, `double2/3/4`, `int2/3/4`, `uint2/3/4`,
`short2/3/4`, `ushort2/3/4`, `byte2/3/4`, `ubyte2/3/4`, `bool2/3/4`;
`mat2/mat2f`, `mat3/mat3f`, `mat4/mat4f`; `quat/quatf`; `half`.
Headers: `math/vec2.h`, `vec3.h`, `vec4.h`, `mat2.h`, `mat3.h`, `mat4.h`,
`quat.h`, `half.h`, plus helpers `TVecHelpers.h`, `TMatHelpers.h`,
`TQuatHelpers.h`, `fast.h`. Matrices are **column-major** (OpenGL convention).

---

## 7. Utilities: `utils::Entity`, `utils::EntityManager`

- `utils::Entity` (`include/utils/Entity.h`): a lightweight **handle** to an
  object in the entity-component system. `isNull()`, `clear()`, `operator bool`,
  hash support for containers. Passed by value.
- `utils::EntityManager` (`include/utils/EntityManager.h`): process-wide
  singleton — `EntityManager::get()`. `create()` / `create(n, entities)` /
  `destroy(entity)` / `destroy(n, entities)`; `isAlive(entity)`;
  `getMaxEntityCount()` / `getEntityCount()`; `registerListener` /
  `registerChangeCallback` / `flushNotifications`.

Standard pattern (used by the project samples, e.g. the "hello triangle"
flow in the
[iOS sample guide](https://github.com/google/filament/blob/HEAD/docs_src/src_mdbook/src/samples/ios.md)
and the project README):

```cpp
utils::Entity e = utils::EntityManager::get().create();
// ... attach components via managers/builders, add to scene ...
engine->destroy(e);                    // strip Filament components
utils::EntityManager::get().destroy(e); // free the entity id
```

---

## 8. gltfio vs core Filament

### 8.1 What gltfio is

**gltfio** is a higher-level library layered *on top of* core Filament that
loads **glTF 2.0** assets (`.gltf` JSON or `.glb` binary). It parses the file,
creates Filament `VertexBuffer`/`IndexBuffer`/`Texture`/`MaterialInstance`s
and entities with `RenderableManager`/`TransformManager` components, and
optionally drives skeletal/transform **animations**. Core Filament has no
model-file loading of its own.

Supported glTF features (per the
[Filament README](https://github.com/google/filament/blob/main/README.md)):
embedded/binary encodings; points/lines/line loop/line strip/triangles/triangle
strip/triangle fan primitives; transform/linear-interp/morph/sparse-accessor/skin/joint
animation; extensions `KHR_draco_mesh_compression`,
`KHR_lights_punctual`, `KHR_materials_clearcoat`, `KHR_materials_dispersion`,
`KHR_materials_emissive_strength`, `KHR_materials_ior`,
`KHR_materials_pbrSpecularGlossiness`, `KHR_materials_sheen`,
`KHR_materials_specular`, `KHR_materials_transmission`,
`KHR_materials_unlit`, `KHR_materials_variants`, `KHR_materials_volume`,
`KHR_mesh_quantization`, `KHR_texture_basisu`, `KHR_texture_transform`,
`EXT_meshopt_compression`.

### 8.2 Public API classes (all in `filament::gltfio`)

| Class | Header | Role |
|---|---|---|
| `AssetLoader` | `include/gltfio/AssetLoader.h` | Parses glTF content → `FilamentAsset`; owns a material cache |
| `ResourceLoader` | `include/gltfio/ResourceLoader.h` | Uploads buffers/textures to the GPU |
| `MaterialProvider` | `include/gltfio/MaterialProvider.h` | Interface supplying glTF PBR materials (two impls) |
| `FilamentAsset` | `include/gltfio/FilamentAsset.h` | Bundle of Filament resources for one glTF file |
| `FilamentInstance` | `include/gltfio/FilamentInstance.h` | One instanced copy (own entities/components) |
| `Animator` | `include/gltfio/Animator.h` | Drives glTF animations |
| `TextureProvider` | `include/gltfio/TextureProvider.h` | Interface for async texture decode (`createStbProvider`, `createKtx2Provider`) |

**Standard usage flow** (verbatim example from `include/gltfio/AssetLoader.h`):

```cpp
auto engine    = Engine::create();
auto materials = createJitShaderProvider(engine);
auto decoder   = createStbProvider(engine);
auto loader    = AssetLoader::create({engine, materials});

// Parse the glTF content and create Filament entities.
std::vector<uint8_t> content(...);
FilamentAsset* asset = loader->createAsset(content.data(), content.size());
content.clear();

// Load buffers and textures from disk.
ResourceLoader resourceLoader({engine, ".", true});
resourceLoader.addTextureProvider("image/png", decoder);
resourceLoader.addTextureProvider("image/jpeg", decoder);
resourceLoader.loadResources(asset);

// Free the glTF hierarchy as it is no longer needed.
asset->releaseSourceData();

// Add renderables to the scene.
scene->addEntities(asset->getEntities(), asset->getEntityCount());

// Extract the animator interface from the FilamentInstance.
auto animator = asset->getInstance()->getAnimator();

// Execute the render loop and play the first animation.
do {
    animator->applyAnimation(0, time);
    animator->updateBoneMatrices();
    if (renderer->beginFrame(swapChain)) {
        renderer->render(view);
        renderer->endFrame();
    }
} while (!quit);

scene->removeEntities(asset->getEntities(), asset->getEntityCount());
loader->destroyAsset(asset);
materials->destroyMaterials();
delete materials;
delete decoder;
AssetLoader::destroy(&loader);
Engine::destroy(&engine);
```

Details per class:

- **`AssetLoader`** — `static AssetLoader* create(const AssetConfiguration&)`;
  `AssetConfiguration{ engine, materials /*MaterialProvider* owned by client*/,
  names /*NameComponentManager* optional*/, entities /*EntityManager* optional*/,
  defaultNodeName, ext /*mikktspace tangents, disk-local glTF only*/ }`.
  `createAsset(bytes, numBytes)` (one instance),
  `createInstancedAsset(bytes, numBytes, instances, numInstances)`,
  `createInstance(asset)`, `destroyAsset(asset)` (frees entities, components,
  material instances, buffers, textures), `enableDiagnostics(bool)`,
  `getMaterials()/getMaterialsCount()` (weak refs to cached materials),
  `getNodeManager()`, `getMaterialProvider()`, `gc()`.
  `static void destroy(AssetLoader**)`.
- **`ResourceLoader`** — `explicit ResourceLoader(const ResourceConfiguration&)`;
  `ResourceConfiguration{ engine, gltfPath /*deprecated*/, normalizeSkinningWeights }`.
  `addResourceData(uri, BufferDescriptor&&)` (external buffers),
  `hasResourceData(uri)`, `evictResourceData()`,
  `addTextureProvider(mimeType, TextureProvider*)`,
  `loadResources(asset)` (blocking), or async:
  `asyncBeginLoad(asset)` / `asyncGetLoadProgress()` / `asyncUpdateLoad()` /
  `asyncCancelLoad()`.
- **`MaterialProvider`** — interface with
  `createMaterialInstance(MaterialKey* config, UvMap* uvmap, label, extras)`
  (creates/fetches compiled material, returns instance),
  `getMaterial(...)`, `getMaterials()/getMaterialsCount()`,
  `destroyMaterials()`, `needsDummyData(VertexAttribute)`.
  Two implementations:
  - `createJitShaderProvider(engine, optimizeShaders, variantFilters)` — generates
    materials at runtime via **filamat** (streamlined shaders, slower startup).
    *"Requires `libfilamat` to be linked in. Not available in `libgltfio_core`."*
  - `createUbershaderProvider(engine, archive, archiveByteCount)` — small set of
    pre-built materials; no runtime compilation.
  Cached materials are **not** freed automatically when the provider is
  destroyed (clients may take ownership); call `destroyMaterials()` explicitly.
- **`FilamentAsset`** — `getEntities()/getEntityCount()`,
  `getRenderableEntities()/getRenderableEntityCount()`,
  `getLightEntities()`, `getCameraEntities()`, `getRoot()`,
  `popRenderable()/popRenderables(...)` (incremental scene insertion),
  `getResourceUris()/getResourceUriCount()`, `getBoundingBox()`,
  `getInstance()` (primary `FilamentInstance`), `getAnimator()`,
  `getName(entity)`, `getExtras(entity)`, `getSceneCount()/getSceneName(i)`,
  `addEntitiesToScene(scene, entities, count, sceneIndex)`,
  `releaseSourceData()` (frees glTF JSON hierarchy after load),
  `detachFilamentComponents()`.
- **`FilamentInstance`** — `getEntities()/getEntityCount()`, `getRoot()`,
  `getAnimator()`, `getMaterialVariantCount()/getMaterialVariantName(i)/
  applyMaterialVariant(i)`, `getSkinCount()/getSkinNameAt/getJointCountAt/
  getJointsAt/attachSkin/detachSkin`, `recomputeBoundingBoxes()` (after
  `loadResources`), `getMaterialInstances()`.
- **`Animator`** — `applyAnimation(index, time)`,
  `updateBoneMatrices()`, `applyCrossFade(prevIndex, prevTime, alpha)`,
  `resetBoneMatrices()`, `getAnimationCount()`,
  `getAnimationDuration(index)`, `getAnimationName(index)`.
- **`TextureProvider`** — async decode interface:
  `pushTexture(data, byteCount, mimeType, …)` / `popTexture()` /
  `updateQueue()` / `waitForCompletion()` / `cancelDecoding()` /
  `getPushedCount()/getPoppedCount()/getDecodedCount()` /
  `getPushMessage()/getPopMessage()` (error strings).
  Public factories: `createStbProvider(engine)` (PNG/JPEG via stb),
  `createKtx2Provider(engine)` (KTX2/BasisU).

### 8.3 gltfio internals (public headers, but not user-facing)

- **`NodeManager`** (`include/gltfio/NodeManager.h`) — "Node components are
  created by gltfio and exposed to users to allow **inspection**." Useful for
  reading glTF node info; do not create components yourself.
- **`TrsTransformManager`** (`include/gltfio/TrsTransformManager.h`) —
  "Trs information here just used for **Animation**, DON'T use for transform."
- `struct FFilamentAsset` forward-declared in `ResourceLoader.h`
  (implementation detail); `Animator::addInstance()` marked
  *"For internal use only."*

---

## 9. Threading model

All from header doc comments (verify against your target version):

- **Engine is not thread-safe.** "The implementation makes no attempt to
  synchronize calls to an Engine instance methods. If multi-threading is
  needed, synchronization must be external."
  (`include/filament/Engine.h`, "Thread safety").
- On creation, the Engine starts **a render thread** (which talks to the GPU
  driver) plus **worker threads** for its `JobSystem` (elevated priority;
  count heuristic, configurable via `Config::jobSystemThreadCount`).
- **Main thread** = the thread that calls `Renderer::beginFrame`
  (`include/filament/Stream.h`). All `Engine` API calls happen from the main
  thread; the render thread is Filament-internal.
- `Renderer::render()` "must be called from the Engine's main thread (or
  external synchronization must be provided). In particular, calls to
  `render()` on different Renderer instances **must** be synchronized."
  (`include/filament/Renderer.h`).
- `Renderer::renderStandaloneView()` must likewise be called from the Engine's
  main thread.
- `Fence` exists precisely "to synchronize the application main thread with
  filament's rendering thread" (`include/filament/Fence.h`).
- `Engine::destroy(Engine**)` and `Engine::destroy(Engine*)` are documented
  thread-safe.
- `ResourceLoader` "must be destroyed on the same thread that calls
  `filament::Renderer::render()` because it listens to
  `filament::backend::BufferDescriptor` callbacks"
  (`include/gltfio/ResourceLoader.h`) — those callbacks run on the main
  Filament thread.
- `BufferDescriptor` release callbacks: "Guarantees: called on the main
  filament thread. Limitations: must be lightweight. Must not call filament
  APIs." (`include/backend/BufferDescriptor.h`).
- A custom `backend::Platform` passed to `Engine::Builder::platform()` has
  "all methods called from filament's render thread, which is different from
  the main thread."

---

## 10. Resource lifecycle & destruction ordering

1. **Create** via builders/factories (`Engine::create*`, `Xxx::Builder::build`,
   `Material::createInstance`, `AssetLoader::createAsset`).
2. **Destroy** via `engine->destroy(ptr)` (overloads return `bool`); `Engine`
   tracks everything, and leaked resources are freed at engine destruction
   **with a console warning** — i.e. explicit destruction is expected.
3. **Ordering rules:**
   - Destroy `MaterialInstance`s **before** their `Material`
     (`@attention` on `Engine::destroy(const Material*)`).
   - For entities: `engine->destroy(entity)` strips Filament components
     first, then `utils::EntityManager::get().destroy(entity)` frees the id.
   - Camera components are removed with
     `engine->destroyCameraComponent(entity)` (not `destroy(camera)`).
   - The doc-comment render loop destroys in reverse creation order:
     `view → scene → renderer → swapChain → Engine::destroy(&engine)`.
   - `Engine::destroy()` "should be called last and after all other resources
     have been destroyed."
4. **gltfio:** `loader->destroyAsset(asset)` frees entities, components,
   material instances, buffers and textures of the asset;
   `MaterialProvider::destroyMaterials()` is **not** automatic — call it
   explicitly (or take ownership of the cache).
5. Validation: `engine->isValid(ptr)` per type;
   `getXxxCount()` debug counters exist for leak hunting.

---

## 11. Gotchas

1. **Builders are short-lived.** Doc comment on `RenderableManager::Builder`:
   "builders typically do not have a long lifetime since clients should
   discard them after calling build()." `RenderableManager::Builder` is
   move-only (copy ctor deleted); all builders derive from
   `BuilderBase<Details>` = `utils::PrivateImplementation<T>`. Don't stash a
   builder and reuse it across frames.
2. **`VertexAttribute::TANGENTS` is a quaternion.** It must be supplied as
   `float4` encoding tangent+bitangent+normal — not as separate normal/tangent
   vectors (`include/filament/VertexBuffer.h` warning).
3. **3-component non-float attributes are not portable.** "Not all backends
   support 3-component attributes that are not floats. For help with
   conversion, see `geometry::Transcoder`." (`include/filament/VertexBuffer.h`).
4. **`VertexBuffer::Builder::bufferCount()` is mandatory** (default 0, max 8);
   forgetting it yields an empty/failed buffer. `setBufferAt` byte offsets
   must be multiples of 4.
5. **`BufferDescriptor`/`PixelBufferDescriptor` own their memory and are
   move-only.** The release callback fires on Filament's main thread and
   **must not call Filament APIs**. A common pattern is a `delete[]` lambda;
   forgetting the callback (or using `nullptr` with non-`malloc`'d memory)
   leaks or crashes.
6. **`Material::Builder::build()` can return `nullptr`** — always null-check.
   The `package()` payload only needs to stay valid until `build()` returns.
7. **Destroy instances before the material.** `Engine::destroy(Material*)`
   requires all its `MaterialInstance`s to be gone first.
8. **Engine is not thread-safe.** Do all API calls from the thread that runs
   `beginFrame` (the "main thread"); use `Fence` to synchronize with the
   render thread; never call `Fence::wait(..., DONT_FLUSH)` from a thread that
   could be executing the fenced work.
9. **Feature levels gate features.** `SAMPLER_CUBEMAP_ARRAY` needs feature
   level ≥ 2; some formats/Usages are backend-dependent — probe with
   `Texture::isTextureFormatSupported` / `isTextureFormatMipmappable` /
   `isTextureSwizzleSupported` before relying on them.
10. **matc target must match runtime.** Compile `.mat` files with the same
    Filament version and the right `-p` (desktop/mobile) / `-a` (opengl/vulkan)
    targets; `matc` validates shaders at compile time and reports `.mat`
    line numbers.
11. **`Material::getRequiredAttributes()`** tells you which vertex attributes a
    compiled shader actually reads — use it to keep `VertexBuffer` layouts in
    sync with custom `.mat` files.
12. **Default instances are shared.** `Material::getDefaultInstance()` returns
    the material's shared instance; `setParameter` on it affects every
    renderable using the default instance — use `createInstance()` for
    per-object parameters.
13. **gltfio's `MaterialProvider` cache is manual.** `destroyMaterials()` is
    not called by the provider destructor; skipping it leaks GPU programs.
    `createJitShaderProvider` needs `libfilamat` linked
    (absent from `libgltfio_core`).
14. **`ResourceLoader` threading.** Destroy it on the same thread that calls
    `Renderer::render()` (it consumes `BufferDescriptor` callbacks).
15. **`Renderer::readPixels` is asynchronous** — data arrives via the
    `PixelBufferDescriptor` callback on the main thread, typically a few
    frames later; don't treat it as synchronous.
16. **Clear color vs tone mapping.** `Renderer::ClearOptions::clearColor` is
    applied *without* tone mapping; for opaque views prefer a `Skybox` (or
    black/transparent) over a mid-gray clear color, which would look different
    from in-view rendering of the same color.

---

## 12. Smaller but useful APIs

- **`filament::Box`** (`include/filament/Box.h`) — `{min, max}` AABB used for
  renderable bounding boxes; `filament::Aabb` (float) / `filament::Box`
  (double) variants.
- **`BufferObject`** (`include/filament/BufferObject.h`) — shareable GPU
  buffer backing for `VertexBuffer`/`IndexBuffer` in buffer-object mode.
- **`RenderTarget`** (`include/filament/RenderTarget.h`) — offscreen rendering;
  `Builder().texture(AttachmentPoint::{COLOR, COLOR0..7, DEPTH, STENCIL,
  …}, texture).mipLevel(...).face(...).layer(...).build(engine)`.
- **`IndirectLight`** (`include/filament/IndirectLight.h`) — image-based
  lighting: `Builder().reflections(cubemapTexture).irradiance(bands, sh) |
  .radiance(bands, sh) | .irradiance(cubemap).intensity(f).rotation(mat3f).
  build(engine)`; `setIntensity`/`getIntensity`.
- **`Skybox`** (`include/filament/Skybox.h`) — `Builder().environment(cubemap).
  showSun(bool).intensity(f).color(float4).priority(n).build(engine)`;
  `setColor`, `setLayerMask`.
- **`ColorGrading`** (`include/filament/ColorGrading.h`) —
  `Builder().toneMapping(ToneMapping::{LINEAR, ACES_LEGACY, ACES, FILMIC,
  AGX, GENERIC, PBR_NEUTRAL, GT7, …}).exposure(f).nightAdaptation(f).
  whiteBalance(...).channelMixer(...).shadowsMidtonesHighlights(...).
  slopeOffsetPower(...).contrast(f).vibrance(f).saturation(f).
  curves(...).luminanceScaling(...).gamutMapping(...).build(engine)`;
  attached per-view via `View::setColorGrading`.
- **`Stream`** (`include/filament/Stream.h`) — video/external image streams;
  `Builder().stream(intptr_t)` (deprecated), `.width/.height` variants.
- **`MorphTargetBuffer` / `SkinningBuffer`** — GPU morph/skin data backends
  (`include/filament/MorphTargetBuffer.h`, `SkinningBuffer.h`).
- **`InstanceBuffer`** (`include/filament/InstanceBuffer.h`) — GPU instancing
  data.
- **`DebugRegistry`** (`include/filament/DebugRegistry.h`) —
  `engine->getDebugRegistry()`; runtime boolean debug flags shared between
  main and backend threads.
- **`filament::Exposure`** (`include/filament/Exposure.h`) — helpers mapping
  EV100/aperture/shutter/ISO to exposure.
- **`camutils`** (`include/camutils/`) — camera manipulators (orbit, map,
  flight) used by samples/viewers.
- **`geometry::Transcoder`** (`include/geometry/Transcoder.h`) — convert
  vertex data to backend-friendly layouts (e.g. 3-component int → float).
- **`image` / `imageio`** (`include/image/`, `include/imageio-lite/`) —
  CPU image containers and codecs (used by tools and `TextureProvider`s).
- **`ktxreader`** (`include/ktxreader/`) — KTX container parsing
  (basis of `createKtx2Provider`).
- **`viewer`** (`include/viewer/`) — `viewer::Settings`, automation/JSON
  settings used by `gltf_viewer`-style apps.
- **Tools** (in `bin/`): `matc` (material compiler), `cmgen` (IBL
  prefiltering → cubemap + spherical harmonics), `matinfo` (inspect compiled
  materials), `filamesh` (convert meshes), plus sample apps
  (`gltf_viewer`, `sample_hello_triangle`, …) under
  `samples/` in https://github.com/google/filament.

---

## Sources

- Prebuilt v1.77.0 headers: `include/filament/*.h`, `include/backend/*.h`,
  `include/utils/*.h`, `include/math/*.h`, `include/gltfio/*.h`,
  `include/filamat/MaterialBuilder.h` in the distribution root.
- [Filament Materials Guide](https://google.github.io/filament/Materials.md.html)
  (material definitions, material packages, `matc` flags).
- [Filament README](https://github.com/google/filament/blob/main/README.md)
  (backends, features, glTF support list, basic render loop).
- [iOS sample guide](https://github.com/google/filament/blob/HEAD/docs_src/src_mdbook/src/samples/ios.md)
  (end-to-end flow: engine → swapchain → camera → view → renderable → material).
- Official docs site: https://google.github.io/filament/
