# Cesium Native v0.64.0 — API Reference

**Version researched:** v0.64.0 (per `CHANGES.md`: "### v0.64.0 - 2026-09-01").
**Source:** `~/workspace/3dtiles-renderer/build/linux/_deps/cesium_native-src/`
**Official docs:** https://cesium.com/learn/cesium-native/ · GitHub: https://github.com/CesiumGS/cesium-native

> **Conventions used in this document**
> - Every symbol is cited with its header path, relative to the source root above (e.g. `CesiumUtility/include/CesiumUtility/IntrusivePointer.h`).
> - "Generated" headers live under `<Module>/generated/include/Cesium<Module>/` and are schema-driven (glTF / 3D Tiles JSON specs); they are plain structs with `std::optional` fields and no behavior.
> - "Hand-written" headers live under `<Module>/include/Cesium<Module>/` and contain the real logic.
> - Symbols under `Impl/`, `detail`, or documented as internal ("Not supposed to be used by clients") are flagged as **internal**.
> - Library import macros (`CESIUMUTILITY_API`, `CESIUMASYNC_API`, …) are omitted from signatures for readability; classes are exported from their DLL/shared library unless noted.

---

## Table of Contents

1. [CesiumUtility](#1-cesiumutility) — foundation: smart pointers, results/errors, JSON values, URIs, math, credits, gzip
2. [CesiumAsync](#2-cesiumasync) — async system, futures, asset accessors, caching
3. [CesiumGeometry](#3-cesiumgeometry) — bounding volumes, tiling schemes, transforms
4. [CesiumGeospatial](#4-cesiumgeospatial) — ellipsoid, cartographic coordinates, projections
5. [CesiumJsonReader / CesiumJsonWriter](#5-cesiumjsonreader--cesiumjsonwriter) — streaming JSON framework
6. [CesiumGltf](#6-cesiumgltf) — glTF data model (generated structs + handwritten views)
7. [CesiumGltfReader / CesiumGltfWriter](#7-cesiumgltfreader--cesiumgltfwriter)
8. [CesiumImage](#8-cesiumimage)
9. [Cesium3DTiles / Cesium3DTilesReader / Cesium3DTilesWriter](#9-cesium3dtiles--cesium3dtilesreader--cesium3dtileswriter)
10. [Cesium3DTilesContent](#10-cesium3dtilescontent) — b3dm/i3dm/pnts/cmpt converters
11. [CesiumGltfContent](#11-cesiumgltfcontent)
12. [Cesium3DTilesSelection](#12-cesium3dtilesselection) — tileset traversal engine
13. [CesiumRasterOverlays](#13-cesiumrasteroverlays)
14. [CesiumVectorData / CesiumVectorOverlays](#14-cesiumvectordata--cesiumvectoroverlays)
15. [CesiumQuantizedMeshTerrain](#15-cesiumquantizedmeshterrain)
16. [CesiumIonClient](#16-cesiumionclient)
17. [CesiumITwinClient](#17-cesiumitwinclient)
18. [CesiumClientCommon](#18-cesiumclientcommon)
19. [CesiumCurl](#19-cesiumcurl)
20. [Global Gotchas](#20-global-gotchas)

---

## 1. CesiumUtility

**Purpose.** The lowest-level module; everything else depends on it. Provides reference-counted smart pointers, the `Result<T>`/ErrorList error pattern, a generic `JsonValue` DOM, URI utilities, math helpers, a credit (attribution) system, gzip, hashing, and logging glue. No dependencies outside the STL plus spdlog/glm/rapidjson.

### 1.1 Object lifetime: `IntrusivePointer` and `ReferenceCounted`

- `CesiumUtility/include/CesiumUtility/IntrusivePointer.h` — `template <typename T> class IntrusivePointer final`. A smart pointer that calls `addReference()` / `releaseReference()` on the controlled object. Copy increments, move transfers, destruction releases. Header doc warns: **thread-safety depends entirely on the implementation of `addReference`/`releaseReference` on the pointee** — objects not meant for multithreaded use are unsafe to share via `IntrusivePointer` across threads.
  - Construction/assignment from raw `T*` is implicit; `emplace(...)` constructs in place; `get()`, `operator->`, `operator*`, `operator bool`, `reset()`, comparison operators; `const_intrusive_cast<T>(p)` for const-casts.
- `CesiumUtility/include/CesiumUtility/ReferenceCounted.h` — `template <typename T, bool isThreadSafe = true> class ReferenceCounted`. CRTP base: declare `class MyClass : public ReferenceCounted<MyClass>`. Provides `addReference() const`, `releaseReference() const` (deletes the object at count zero), `getReferenceCount() const`.
  - `ReferenceCountedThreadSafe<T>` = `ReferenceCounted<T, true>` (default); `ReferenceCountedNonThreadSafe<T>` = `ReferenceCounted<T, false>`. **Choose deliberately** — the non-thread-safe variant is cheaper but, per `IntrusivePointer.h` docs, makes cross-thread sharing unsafe.
- `CesiumUtility/include/CesiumUtility/SharedAsset.h` — `class SharedAsset : public ExtensibleObject`. Base class for assets shared between objects (e.g. `ImageAsset` derives from `SharedAsset<ImageAsset>` — see `CesiumImage/include/CesiumImage/ImageAsset.h`). Lifetime managed by reference counting via `SharedAssetDepot` (`CesiumAsync/include/CesiumAsync/SharedAssetDepot.h`); `SharedAssetDepot` is a **friend** and manipulates `addReference(bool threadOwnsDepotLock)`. Also carries `_isInvalidated` flag. Consumers generally use `IDepotOwningAsset.h` types rather than touching this directly.

### 1.2 Error handling: `ErrorList` and `Result<T>`

- `CesiumUtility/include/CesiumUtility/ErrorList.h` — `struct ErrorList` with two public vectors: `std::vector<std::string> errors; std::vector<std::string> warnings;`. Factories `static ErrorList error(std::string)` / `static ErrorList warning(std::string)`; `merge(const ErrorList&)`, `emplaceError(...)`, `emplaceWarning(...)`, `hasErrors()`, `explicit operator bool()` (true if any errors), and logging helpers `logError/logWarning/log(pLogger, prompt)` plus `format(prompt)` using spdlog.
- `CesiumUtility/include/CesiumUtility/Result.h` — `template <typename T> struct Result { std::optional<T> value; ErrorList errors; }`. **Contract (from header docs):** if a value is provided there should be no *errors* (warnings are OK); if no value is provided there must be at least one error. Specialization `Result<IntrusivePointer<T>>` stores `IntrusivePointer<T> pValue` instead of `std::optional`. Alias `template <typename T> using ResultPointer = Result<IntrusivePointer<T>>`.
  - Note: this is the *value-or-diagnostics* pattern used by synchronous APIs. Async rejection uses C++ exceptions on `Future` instead (see §2).

### 1.3 `JsonValue`

- `CesiumUtility/include/CesiumUtility/JsonValue.h` — `class JsonValue final`: a variant-based DOM for arbitrary JSON, used for `extras` fields in generated models. Supports null/bool/int/uint/double/string/array/object. (Only verified to exist and be used for `extras`; detailed accessor API not exhaustively read.)

### 1.4 URI utilities

- `CesiumUtility/include/CesiumUtility/Uri.h` — `class Uri`: parse/validate URIs (`isValid()`, `getScheme()`, `getHost()`, `getPath()`, `getFileName()`, `getStem()`, `getExtension()`, `getQuery()`, `setPath()`, `setQuery()`), plus statics:
  - `static std::string resolve(const std::string& base, const std::string& relative)` — resolves relative URLs against a base (used everywhere external references are followed).
  - `static std::string addQuery(const std::string& uri, const std::string& key, const std::string& value)`
  - `static std::string substituteTemplateParameters(const std::string& templateUri, ...)` — used for implicit-tiling `{level}/{x}/{y}` style templates.
  - `static std::string escape(const std::string& s)` / `unescape(...)`; `unixPathToUriPath`, `windowsPathToUriPath`, `nativePathToUriPath`.

### 1.5 Credits (attribution)

- `CesiumUtility/include/CesiumUtility/CreditSystem.h` — `class CreditSystem`, `struct Credit` (opaque id+generation, only constructible by `CreditSystem`/`CreditReferencer`), `class CreditSource` (identifies one attribution source; obtained from a `CreditSystem`). `enum class CreditFilteringMode : uint8_t`. Pattern: create a `CreditSystem`, hand `CreditSource`s to tilesets/overlays, then periodically ask the system which credits are currently shown on screen vs. should be removed. `Tileset::getTilesetCredits()` / `getUserCredit()` / `setShowCreditsOnScreen(bool)` in §12 tie into this.

### 1.6 Misc utilities

- `CesiumUtility/include/CesiumUtility/Math.h` — constants and helpers (epsilon comparisons, conversions). Not exhaustively read.
- `CesiumUtility/include/CesiumUtility/Gzip.h` — free functions: `bool isGzip(span)`, `bool gzip(span, vector&)`, `bool gunzip(...)`. Used by `GunzipAssetAccessor` (§2).
- `CesiumUtility/include/CesiumUtility/Hash.h`, `StringHelpers.h`, `joinToString.h`, `ScopeGuard.h`, `DoublyLinkedList.h`, `SpanHelper.h`, `TransformIterator.h`, `TreeTraversalState.h`, `AttributeCompression.h` (oct-encoding etc. for quantized attributes), `DerivedValue.h`, `Assert.h`, `Log.h`, `Tracing.h` (compile-time tracing hooks, `CESIUM_TRACING_ENABLED`), `ExtensibleObject.h` (base with `extensions`/`extras` for generated models).

### Gotchas — CesiumUtility

- `Result<T>::value` is `std::optional` — always check before dereferencing; the presence of warnings does **not** imply failure.
- `IntrusivePointer` is `final` and implicitly convertible to raw pointer — easy to accidentally keep a raw pointer past the last owner's destruction. Treat raw pointers obtained from it as non-owning.
- `ReferenceCounted<T,false>` (via `ReferenceCountedNonThreadSafe`) silently opts out of atomic refcounts; sharing such objects across threads is a data race (per `IntrusivePointer.h` doc).
- Generated model structs put `extensions`/`extras` on `ExtensibleObject`; unknown JSON properties land in `extras` as `JsonValue` — parse-tolerant by design.

---

## 2. CesiumAsync

**Purpose.** The async runtime: a `Future`/`Promise` framework with explicit main-thread vs. worker-thread scheduling, plus the asset-download abstraction (`IAssetAccessor`) and response caching. Depends on `CesiumUtility`.

### 2.1 `AsyncSystem` — the scheduler hub

`CesiumAsync/include/CesiumAsync/AsyncSystem.h` — `class AsyncSystem final`. Instances are cheap to copy (shared scheduler state) but **the last `AsyncSystem` must be destroyed only after all continuations have completed**, otherwise continuations may be scheduled on dead schedulers → crash. Two sanctioned patterns per header doc: wait for all futures before destroying the owner, or make the `AsyncSystem` global/static.

Key methods:
```cpp
AsyncSystem(const std::shared_ptr<ITaskProcessor>& pTaskProcessor) noexcept;

template <typename T, typename Func> Future<T> createFuture(Func&& f) const;
// f receives a Promise<T>; may resolve/reject it later; exceptions thrown by
// f automatically reject the Future (more exception-safe than createPromise).

template <typename T> Promise<T> createPromise() const;

template <typename Func> /* Future<...> */ runInWorkerThread(Func&& f) const;
// Runs f on the ITaskProcessor's background thread. If called *from* a worker
// thread, f runs immediately, synchronously.

template <typename Func> /* Future<...> */ runInMainThread(Func&& f) const;
// Queues f on the main thread. If called from the main thread, runs immediately.

template <typename Func> /* Future<...> */ runInThreadPool(const ThreadPool& threadPool, Func&& f) const;

template <typename T> Future</* vector<T> or void */> all(std::vector<Future<T>>&& futures) const;
template <typename T> Future<...> all(std::vector<SharedFuture<T>>&& futures) const;
template <typename... Futures> Future<std::tuple<...>> all(Futures&&... futures) const;
// Rejects if ANY input rejects; rejection carries the first/leftmost exception.
// Tip from docs: attach catchInMainThread first if you need per-future errors.

template <typename T> Future<T> createResolvedFuture(T&& value) const;
Future<void> createResolvedFuture() const;

void dispatchMainThreadTasks();        // run all queued main-thread tasks, now
bool dispatchOneMainThreadTask();     // run a single queued task; false if none
MainThreadScope enterMainThread() const; // RAII: current thread becomes "main"
ThreadPool createThreadPool(int32_t numberOfThreads) const;
```

### 2.2 `Future<T>` — single-consumer async value

`CesiumAsync/include/CesiumAsync/Future.h` — `template <typename T> class Future final`. **Move-only** (copy ctor/assignment deleted); every `then*` is an **rvalue-qualified** `&&` method that consumes/invalidates the future.

```cpp
template <typename Func> /* Future<...> */ thenInWorkerThread(Func&& f) &&;
template <typename Func> /* Future<...> */ thenInMainThread(Func&& f) &&;
template <typename Func> /* Future<...> */ thenImmediately(Func&& f) &&;   // runs in the resolving thread
template <typename Func> /* Future<...> */ thenInThreadPool(const ThreadPool&, Func&& f) &&;
template <typename Func> Future<T> catchInMainThread(Func&& f) &&;
template <typename Func> Future<T> catchImmediately(Func&& f) &&;
// Continuation may return a plain value OR another Future (which is awaited).
// catch* callbacks convert a rejection back into a resolution for downstream thens.

template <typename... TPassThrough>
Future<std::tuple<...>> thenPassThrough(TPassThrough&&... values) &&;

T wait();              // blocks; THROWS the rejection exception. MUST NOT be called
                       // on the main thread (deadlock with dispatchMainThreadTasks).
T waitInMainThread();  // blocks the main thread while pumping main-thread tasks.
bool isReady() const;  // true => wait() will not block
SharedFuture<T> share() &&;  // multi-consumer version; invalidates this Future
```

### 2.3 `Promise<T>` and `SharedFuture<T>`

- `CesiumAsync/include/CesiumAsync/Promise.h` — `template <typename T> class Promise`: `resolve(T)`, `reject(std::exception_ptr)`, `getFuture()`. Obtained via `AsyncSystem::createPromise<T>()` or the callback form `createFuture`.
- `CesiumAsync/include/CesiumAsync/SharedFuture.h` — `template <typename T> class SharedFuture`: copyable, multiple continuations, `wait()`/`waitInMainThread()` like `Future`.

### 2.4 Asset access: `IAssetAccessor`, `IAssetRequest`, `IAssetResponse`

`CesiumAsync/include/CesiumAsync/IAssetAccessor.h`:
```cpp
class IAssetAccessor {
public:
  typedef std::pair<std::string, std::string> THeader;
  virtual ~IAssetAccessor() = default;
  virtual Future<std::shared_ptr<IAssetRequest>> get(
      const AsyncSystem& asyncSystem, const std::string& url,
      const std::vector<THeader>& headers = {}) = 0;
  virtual Future<std::shared_ptr<IAssetRequest>> request(
      const AsyncSystem& asyncSystem, const std::string& verb,
      const std::string& url,
      const std::vector<THeader>& headers = {},
      const std::span<const std::byte>& contentPayload = {}) = 0;
  virtual void tick() noexcept = 0;  // pump while main thread is blocked; no-op if not main-thread dependent
};
```
`CesiumAsync/include/CesiumAsync/IAssetRequest.h` — `method()`, `url()`, `headers()` (all thread-safe), `response()` returning `const IAssetResponse*` or `nullptr` while in flight.
`CesiumAsync/include/CesiumAsync/IAssetResponse.h` — `statusCode()` (uint16_t), `contentType()`, `headers()` (case-insensitive `HttpHeaders` map — `CesiumAsync/include/CesiumAsync/HttpHeaders.h`), `data()` as `std::span<const std::byte>`.
`CesiumAsync/include/CesiumAsync/NetworkAssetDescriptor.h` — `struct NetworkAssetDescriptor { std::string url; std::vector<THeader> headers; ... }` with `loadFromNetwork(...)` → `Future<shared_ptr<IAssetRequest>>` and `loadBytesFromNetwork(...)` → `Future<Result<std::vector<std::byte>>>`.

Implementations / decorators:
- `CesiumAsync/include/CesiumAsync/CachingAssetAccessor.h` — `class CachingAssetAccessor : public IAssetAccessor`: wraps another accessor with an `ICacheDatabase`.
- `CesiumAsync/include/CesiumAsync/SqliteCache.h` — `class SqliteCache : public ICacheDatabase`: SQLite-backed response store, constructed with a database file path (`SqliteCache(...)`); see `ICacheDatabase.h` for the store/load interface.
- `CesiumAsync/include/CesiumAsync/GunzipAssetAccessor.h` — transparently gunzips `Content-Encoding: gzip` responses (uses `CesiumUtility/Gzip.h`).
- `CesiumAsync/include/CesiumAsync/CesiumIonAssetAccessor.h` — injects Cesium ion auth headers into requests.
- `CesiumAsync/include/CesiumAsync/ThreadPool.h` — `class ThreadPool`; `CesiumAsync/include/CesiumAsync/ITaskProcessor.h` — `class ITaskProcessor { virtual void startTask(std::function<void()> f) = 0; }` — **"Not supposed to be used by clients"** per header doc; implemented by the host app/rendering engine to run background work.
- `CesiumAsync/include/CesiumAsync/SharedAssetDepot.h` — depot for `SharedAsset` instances (e.g. shared images across tilesets). Internal-ish; consumers get it via `TilesetExternals::pSharedAssetSystem`.

### Gotchas — CesiumAsync

- **Main-thread pumping is mandatory.** Continuations scheduled with `thenInMainThread` only run when someone calls `AsyncSystem::dispatchMainThreadTasks()` (the `Tileset` does this inside `updateViewGroup`, but standalone futures need manual pumping or `waitInMainThread()`).
- `Future::wait()` on the main thread = deadlock. Use `waitInMainThread()`.
- Futures are single-shot: attach continuations once; use `share()` for multiple consumers.
- Rejections are C++ exceptions (`std::exception_ptr` via `Promise::reject`); unlike `Result<T>`, there is no warnings channel — a rejected future carries exactly one exception.
- `AsyncSystem` lifetime: keep it alive until all futures settle (global/static is the simple safe choice).
- `thenImmediately` runs in *whatever thread resolves the future* — only use for thread-safe, non-blocking work.
- `IAssetAccessor::tick()` exists for accessors that need main-thread pumping (e.g. some platform HTTP stacks); `CurlAssetAccessor` (§19) does its own threading.

### Typical usage (derived from headers + `CesiumAsync/test/ExamplesAsyncSystem.cpp`, `Cesium3DTilesSelection/test/TestTilesetSelection.cpp`)

```cpp
#include <CesiumAsync/AsyncSystem.h>

// Host app provides the background-thread runner:
class MyTaskProcessor : public CesiumAsync::ITaskProcessor {
public:
  void startTask(std::function<void()> f) override {
    std::thread(std::move(f)).detach(); // (real apps use a pool)
  }
};

CesiumAsync::AsyncSystem asyncSystem(std::make_shared<MyTaskProcessor>());
{
  auto scope = asyncSystem.enterMainThread(); // current thread is "main"
  auto future = asyncSystem.runInWorkerThread([]() { return 42; })
      .thenInMainThread([](int v) { /* back on main thread */ });
  int result = future.waitInMainThread(); // pumps main-thread tasks while waiting
}
```

---

## 3. CesiumGeometry

**Purpose.** Pure math/geometry: bounding volumes, culling, ray intersection, quadtree/octree tiling schemes and availability bitstreams, matrix helpers. No I/O, no dependencies beyond glm and CesiumUtility.

### 3.1 Bounding volumes (`CesiumGeometry/include/CesiumGeometry/`)

- `OrientedBoundingBox.h` — `class OrientedBoundingBox`: `center` (dvec3) + `halfAxes` (dmat3). Methods: `getCenter()`, `getHalfAxes()`, `getInverseHalfAxes()`, `getLengths()`, `intersectPlane(const Plane&) → CullingResult`, `contains(dvec3)`, `toAxisAligned() → AxisAlignedBox`, `toSphere() → BoundingSphere`, `static fromSphere(...)`.
- `BoundingSphere.h` — `class BoundingSphere`: center + radius.
- `AxisAlignedBox.h` — `struct AxisAlignedBox`: minimum/maximum corners.
- `BoundingCylinderRegion.h` — cylinder region (used by `3DTILES_bounding_volume_cylinder`).
- `CullingVolume.h` / `GeneralCullingVolume.h` (selection module) — frustum planes; `CullingResult` enum (`Inside`/`Intersecting`/`Outside`) in `CullingResult.h`.
- `Plane.h`, `Ray.h`, `Rectangle.h` (2D, radians), `Axis.h`.
- `IntersectionTests.h` — ray/plane/sphere/box intersection helpers.
- `Transforms.h` — `struct Transforms` with axis-conversion matrices (`Y_UP_TO_Z_UP`, `Z_UP_TO_Y_UP`, `X_UP_TO_Z_UP`, `Z_UP_TO_X_UP`, `X_UP_TO_Y_UP`, `Y_UP_TO_X_UP`), `createTranslationRotationScaleMatrix`, `computeTranslationRotationScaleFromMatrix`, `createViewMatrix`, `createPerspectiveMatrix`, `createOrthographicMatrix`, `getUpAxis` helpers.
- `clipTriangleAtAxisAlignedThreshold.h` — triangle clipping used by quantized-mesh upsampling.

### 3.2 Tiling schemes & availability

- `QuadtreeTilingScheme.h` / `OctreeTilingScheme.h` — map tile IDs (`QuadtreeTileID.h`: level/x/y; `OctreeTileID.h`: level/x/y/z) to rectangles/volumes and child/parent relationships.
- `Availability.h`, `QuadtreeAvailability.h`, `OctreeAvailability.h`, `QuadtreeRectangleAvailability.h` — compact bitstream availability (which tiles exist), mirroring the 3D Tiles implicit-tiling `availableLevels`/subtree bitstreams.
- `TileAvailabilityFlags.h` — flags enum.

**Internal:** anything under `CesiumGeometry/include/CesiumGeometry/` without `Library.h` export concerns is public; no `Impl/` dir was observed in this module's public tree.

### Gotchas — CesiumGeometry

- `Rectangle` angles are in **radians**, longitude in [-π, π], latitude in [-π/2, π/2] (verify per use; `GlobeRectangle` in §4 is the geospatial counterpart).
- glTF is Y-up; 3D Tiles / ECEF is Z-up — `Transforms::Y_UP_TO_Z_UP` exists precisely for this; forgetting it is the classic misplaced-model bug.
- `OrientedBoundingBox::halfAxes` columns are the box axes *scaled by half-lengths*, not unit vectors.

---

## 4. CesiumGeospatial

**Purpose.** Earth-referenced math: ellipsoid conversions, cartographic coordinates, map projections, S2 cells, globe anchors. Depends on `CesiumGeometry`, `CesiumUtility`.

### 4.1 `Ellipsoid`

`CesiumGeospatial/include/CesiumGeospatial/Ellipsoid.h` — `class Ellipsoid final`:
```cpp
static const Ellipsoid WGS84;       // radii (6378137.0, 6378137.0, 6356752.3142451793)
static const Ellipsoid UNIT_SPHERE;
constexpr Ellipsoid(double x, double y, double z) noexcept;
constexpr const glm::dvec3& getRadii() const noexcept;
glm::dvec3 geodeticSurfaceNormal(const glm::dvec3& position) const noexcept;
glm::dvec3 geodeticSurfaceNormal(const Cartographic& cartographic) const noexcept;
glm::dvec3 cartographicToCartesian(const Cartographic& cartographic) const noexcept;
std::optional<Cartographic> cartesianToCartographic(const glm::dvec3& cartesian) const noexcept; // returns nullopt at/near the ellipsoid center
glm::dvec3 scaleToGeodeticSurface(const glm::dvec3& cartesian) const noexcept;
glm::dvec3 scaleToGeocentricSurface(const glm::dvec3& cartesian) const noexcept;
constexpr double getMaximumRadius() const noexcept;
constexpr double getMinimumRadius() const noexcept;
```
(Cartographic/cartesian conversions are ECEF, Z-up, meters.)

### 4.2 Coordinates & projections

- `Cartographic.h` — `class Cartographic final { double longitude; double latitude; double height; }` — **radians, radians, meters**. `Cartographic(longitudeRadians, latitudeRadians, heightMeters=0)`, `static fromDegrees(lon, lat, h=0)`.
- `GlobeRectangle.h` — `class GlobeRectangle`: west/south/east/north in radians.
- `Projection.h` (abstract), `GeographicProjection.h` (equirectangular), `WebMercatorProjection.h` — `project(cartographic) → dvec2` / `unproject(dvec2) → Cartographic`.
- `BoundingRegion.h` — globe rectangle + min/max height, with `BoundingRegionBuilder.h` and `BoundingRegionWithLooseFittingHeights.h` variants.
- `EllipsoidTangentPlane.h`, `LocalHorizontalCoordinateSystem.h` — east-north-up frames at a point.
- `GlobeAnchor.h` — keeps an object fixed to the globe under ellipsoid changes.
- `GlobeTransforms.h` — e.g. east-north-up to fixed-frame matrices.
- `S2CellID.h`, `S2CellBoundingVolume.h` — S2 geometry bindings.
- `CartographicPolygon.h` — polygon on the globe (used by `RasterizedPolygonsTileExcluder` / clipping).
- `SimplePlanarEllipsoidCurve.h` — interpolation between two cartographics on the surface.
- `EarthGravitationalModel1996Grid.h` — EGM96 geoid grid (mean-sea-level ↔ ellipsoid height).
- `calcQuadtreeMaxGeometricError.h` — helper for terrain geometric error.

### Gotchas — CesiumGeospatial

- **Units discipline:** `Cartographic` is radians + meters; `fromDegrees` is the explicit opt-in for degrees. Mixing them silently places cameras on the wrong continent.
- `cartesianToCartographic` returns `std::optional<Cartographic>` — it can fail (e.g. at the ellipsoid center); check it.
- Heights in `sampleHeightMostDetailed` (§12) are **meters above the ellipsoid**, not above mean sea level — use `EarthGravitationalModel1996Grid` if you need MSL.
- `Ellipsoid::WGS84` is the default in `ViewState` and `TilesetOptions::ellipsoid`; custom ellipsoids (Moon, Mars) are supported but raster overlays assume WGS84-ish tiling.

### Typical usage

```cpp
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumGeospatial/Cartographic.h>

const auto& wgs84 = CesiumGeospatial::Ellipsoid::WGS84;
CesiumGeospatial::Cartographic nyc =
    CesiumGeospatial::Cartographic::fromDegrees(-74.006, 40.7128, 10.0);
glm::dvec3 ecef = wgs84.cartographicToCartesian(nyc); // Z-up ECEF, meters
```

---

## 5. CesiumJsonReader / CesiumJsonWriter

**Purpose.** A streaming, SAX-style JSON framework (built on rapidjson) that powers all generated `*Reader`/`*Writer` classes. `CesiumJsonReader` parses without building a DOM; `CesiumJsonWriter` emits JSON with extension hooks.

### CesiumJsonReader (`CesiumJsonReader/include/CesiumJsonReader/`)

- `IJsonHandler.h` — the SAX interface: `readNull()`, `readBool(bool)`, `readInt32/Uint32/Int64/Uint64`, `readDouble(double)`, `readString(string_view)`, `readObjectStart()`, `readObjectKey(string_view)`, `readObjectEnd()`, `readArrayStart()`, `readArrayEnd()`, `reportWarning(...)`. Each returns the next `IJsonHandler*` (or `nullptr`/self to stay). Handlers are typically stack-allocated and chained.
- Concrete handlers: `BoolJsonHandler.h`, `IntegerJsonHandler.h`, `DoubleJsonHandler.h`, `StringJsonHandler.h`, `ArrayJsonHandler.h`, `DictionaryJsonHandler.h`, `ObjectJsonHandler.h` (reflection-ish object mapping), `ExtensibleObjectJsonHandler.h` (handles `extensions`/`extras`), `ExtensionsJsonHandler.h`, `IExtensionJsonHandler.h` (per-extension parsing), `IgnoreValueJsonHandler.h`, `JsonObjectJsonHandler.h`, `SharedAssetJsonHandler.h`, `JsonHandler.h` (base with default no-op behavior).
- `JsonReader.h` — `class JsonReader`: `template <typename T> static ReadJsonResult<T> readJson(span, const TJsonHandlerAdaptor&)`-style entry points (see header for exact template overloads; `internalRead` is static/internal), plus `JsonReaderOptions.h` (`getExtensions()` registry — `GltfReader::getExtensions()` exposes it for glTF).
- Result type pattern: readers return a result struct with `std::optional<T> value`, `errors`, `warnings` (mirrors `GltfReaderResult` in §7).

### CesiumJsonWriter (`CesiumJsonWriter/include/CesiumJsonWriter/`)

- `JsonWriter.h` — `class JsonWriter`: low-level streaming writer (start/end object/array, keys, primitives).
- `PrettyJsonWriter.h` — indented output.
- `ExtensionWriterContext.h` — registry consulted by generated writers to serialize extensions.
- `writeJsonExtensions.h` — helpers for writing `extensions`/`extras`.

**Internal vs public:** `IJsonHandler` and friends are public API (needed to write custom extension handlers); rapidjson types leak through handler signatures — consumers need rapidjson headers on the include path.

### Gotchas

- Streaming means **no random access**: a handler must decide what to do with each value as it arrives; buffering is the handler's job.
- `readString` receives a `string_view` valid only for the duration of the call — copy it if you need it later.
- Unknown properties in extensible objects are captured into `extras` (`JsonValue`), not dropped — but only when using `ExtensibleObjectJsonHandler`.

---

## 6. CesiumGltf

**Purpose.** The glTF 2.0 data model: generated C++ structs for every spec object plus handwritten typed views over accessors and `EXT_structural_metadata`. Depends on `CesiumUtility`.

### 6.1 Generated structs (`CesiumGltf/generated/include/CesiumGltf/`)

One struct per spec object, all in namespace `CesiumGltf`, all deriving from `CesiumUtility::ExtensibleObject` (so they carry `extensions` and `extras`). Naming: spec `accessor` → `AccessorSpec` (because handwritten `Accessor` extends it), `buffer` → `BufferSpec`, `image` → `ImageSpec`, `enum` → `EnumSpec`, etc. Key fields are `std::optional` where the spec says optional, with spec defaults otherwise (e.g. `ModelSpec.h`: `int32_t scene = -1;`, `std::vector<std::string> extensionsUsed/extensionsRequired;`, vectors of `accessors`, `bufferViews`, `buffers`, `images`, `materials`, `meshes`, `nodes`, `samplers`, `scenes`, `skins`, `textures`).

Extension structs are also generated: `ExtensionCesiumRTC.h`, `ExtensionExtMeshGpuInstancing.h`, `ExtensionKhrDracoMeshCompression.h`, `ExtensionExtStructuralMetadata.h`, `ExtensionKhrGaussianSplatting.h`, `ExtensionExtImplicitEllipsoidRegion.h`, `ExtensionExtImplicitCylinderRegion.h`, `ExtensionExtPrimitiveVoxels.h`, `ExtensionExtMeshFeatures.h`, `ExtensionExtInstanceFeatures.h`, `ExtensionKhrTextureTransform`-related (`KhrTextureTransform.h` is handwritten), etc. Consult the directory listing for the full set — it tracks the spec plus Cesium extensions.

**Internal:** the `generated/` tree is public API (consumers read/write these structs directly), but do not hand-edit: it's produced by `tools/generate-classes` from the spec JSON.

### 6.2 Handwritten model (`CesiumGltf/include/CesiumGltf/`)

- `Model.h` — `struct Model : public ModelSpec` plus helpers (e.g. `getSafe` index guards; exact helpers — verify in header before use). The central object: everything hangs off its vectors.
- `Accessor.h` — `struct Accessor final : public AccessorSpec` with `static int8_t computeNumberOfComponents(const std::string& type)`, `static int8_t computeByteSizeOfComponent(int32_t componentType)`.
- `AccessorView.h` — `template <typename T> class AccessorView`: typed, bounds-checked view over an accessor's buffer data. `enum class AccessorViewStatus { Valid, InvalidAccessorIndex, InvalidBufferViewIndex, InvalidBufferIndex, AccessorOutOfBounds, BufferViewOutOfBounds, ... }` — **always check `status()`**; an invalid view must not be dereferenced. Specializations exist for interleaved/quantized types (`PositionAccessorType`, `NormalAccessorType` are variant types since v0.62.0 per CHANGES.md, to support `KHR_mesh_quantization`).
- `AccessorUtility.h`, `AccessorWriter.h`, `getOffsetFromOffsetsBuffer.h` — lower-level accessor helpers.
- `Buffer.h` (`struct Buffer : BufferSpec` + `std::vector<std::byte> cesiumData`), `Image.h` (`struct Image : ImageSpec` + `ImageAsset` payload), `BufferCesium.h` — handwritten payload carriers: `cesiumData` holds the actual bytes (spec structs only hold URIs/byte lengths).
- `NamedObject.h`, `Enum.h`, `KhrTextureTransform.h`, `SamplerUtility.h`, `VertexAttributeSemantics.h`, `InstanceAttributeSemantics.h` — semantic attribute name constants (`POSITION`, `NORMAL`, `TEXCOORD_0`, …).
- `PropertyType.h` / `PropertyTypeTraits.h` — `EXT_structural_metadata` property type system (`PropertyType`, `PropertyComponentType`, conversions from strings).
- `PropertyView.h`, `PropertyTableView.h`, `PropertyTablePropertyView.h`, `PropertyAttributeView.h`, `PropertyAttributePropertyView.h`, `PropertyTextureView.h`, `PropertyTexturePropertyView.h`, `PropertyArrayView.h`, `FeatureIdTextureView.h`, `TextureView.h` — typed views over structural metadata. E.g. `PropertyTableView`: `status()`, `name()`, `size()`, `getClass()`, `getClassProperty(id)`, `getPropertyView<...>(id)`-style accessors, `forEachProperty(callback)`.
- `MetadataConversions.h`, `PropertyTransformations.h` — value normalization (offset/scale, noData handling).
- `ExtensionExtMeshPrimitiveEdgeVisibility.h`, `ExtensionExtPrimitiveVoxels.h` — handwritten parts of newer extensions.

### Gotchas — CesiumGltf

- Spec structs (`*Spec`) vs. extended structs (`Accessor`, `Buffer`, `Image`, `Model`): **use the handwritten ones** — they add `cesiumData` / helpers. E.g. `Model::buffers[i].cesiumData` is where bytes live after load; `BufferSpec` alone has none.
- `AccessorView<T>` with the wrong `T` for the accessor's componentType is a logic error — check `accessor.componentType`/`type` or rely on the status.
- `std::optional` is pervasive in generated structs (e.g. `Node::matrix`, `Node::translation/rotation/scale`); defaults from the spec are materialized as member initializers, so usually you can just read the field.

---

## 7. CesiumGltfReader / CesiumGltfWriter

### CesiumGltfReader

**Purpose.** Parse glTF/GLB from memory or URL into `CesiumGltf::Model`, resolving data URIs, external buffers/images, Draco/meshopt/SPZ decompression, KTX2 transcoding, and structural-metadata schemas. Depends on `CesiumGltf`, `CesiumJsonReader`, `CesiumImage`, `CesiumAsync`, `CesiumUtility`.

`CesiumGltfReader/include/CesiumGltfReader/GltfReader.h`:

```cpp
struct GltfReaderResult {
  std::optional<CesiumGltf::Model> model; // nullopt if the model could not be read
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
};

struct GltfReaderOptions {
  bool decodeDataUrls = true;
  bool clearDecodedDataUrls = true;      // frees URI strings after decode
  bool decodeEmbeddedImages = true;      // via stb_image: JPG/PNG/TGA/BMP/PSD/GIF/HDR/PIC
  bool resolveExternalImages = true;
  bool decodeDraco = true;               // KHR_draco_mesh_compression
  bool decodeMeshOptData = true;         // EXT_meshopt_compression
  bool decodeSpz = true;                 // KHR_gaussian_splatting_compression_spz
  bool dequantizeMeshData = true;        // KHR_mesh_quantization
  bool applyTextureTransform = true;     // KHR_texture_transform
  CesiumImage::Ktx2TranscodeTargets ktx2TranscodeTargets; // target GPU formats per input format
  CesiumUtility::IntrusivePointer<GltfSharedAssetSystem> pSharedAssetSystem = GltfSharedAssetSystem::getDefault();
  bool resolveExternalStructuralMetadata = true; // EXT_structural_metadata schemaUri
  MeshPrimitiveModeOptions primitiveModeOptions; // convert strip/fan/loop -> list
};

class GltfReader {
public:
  GltfReader();
  CesiumJsonReader::JsonReaderOptions& getOptions();       // extension registry
  const CesiumJsonReader::JsonReaderOptions& getExtensions() const;
  GltfReaderResult readGltf(const std::span<const std::byte>& data,
                            const GltfReaderOptions& options = {}) const; // sync, from memory
  CesiumAsync::Future<GltfReaderResult> readGltfAndExternalData(
      const std::span<const std::byte>& data, const CesiumAsync::AsyncSystem& asyncSystem,
      const CesiumAsync::HttpHeaders& headers,
      const std::shared_ptr<CesiumAsync::IAssetAccessor>& pAssetAccessor,
      const std::string& baseUrl = {}, const GltfReaderOptions& options = {}) const;
  // (second overload takes vector<THeader> instead of HttpHeaders)
  CesiumAsync::Future<GltfReaderResult> loadGltf(
      const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url,
      const std::vector<CesiumAsync::IAssetAccessor::THeader>& headers,
      const std::shared_ptr<CesiumAsync::IAssetAccessor>& pAssetAccessor,
      const GltfReaderOptions& options = {}) const;
  // ... postLoad/post-load processing entry point (see header tail)
};
```

- `NetworkImageAssetDescriptor.h`, `NetworkSchemaAssetDescriptor.h` — descriptors for async image/schema fetching via `SharedAssetDepot`.
- `GltfSharedAssetSystem.h` — shared images/schemas depot for glTF loads.

### CesiumGltfWriter

**Purpose.** Serialize `CesiumGltf::Model` back to JSON glTF or binary GLB. (`CesiumGltfWriter/include/CesiumGltfWriter/GltfWriter.h`)

```cpp
struct GltfWriterResult {
  std::vector<std::byte> gltfBytes; // final glTF JSON or GLB bytes
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
};

class GltfWriter {
public:
  CesiumJsonWriter::ExtensionWriterContext& getExtensions();
  const CesiumJsonWriter::ExtensionWriterContext& getExtensions() const;
  GltfWriterResult writeGltf(const CesiumGltf::Model& model,
                             const GltfWriterOptions& options = {}) const;
  // writeGlb: first buffer implicitly refers to the GLB BIN chunk and must not have a uri
  GltfWriterResult writeGlb(const CesiumGltf::Model& model,
                            const std::span<const std::byte>& bufferData,
                            const GltfWriterOptions& options = {}) const;
};
```
`SchemaWriter.h` — writes `EXT_structural_metadata` schemas.

### Typical usage (from `CesiumGltfReader/test/TestGltfReader.cpp`)

```cpp
#include <CesiumGltfReader/GltfReader.h>
CesiumGltfReader::GltfReader reader;
CesiumGltfReader::GltfReaderResult result = reader.readGltf(bytes);
if (result.model) { /* use result.model->meshes etc. */ }
// async with external refs:
reader.readGltfAndExternalData(bytes, asyncSystem, headers, pAssetAccessor, baseUrl)
  .thenInMainThread([](CesiumGltfReader::GltfReaderResult&& r) { ... });
```

### Gotchas

- `readGltf` is synchronous but does **not** resolve external buffers/images — use `readGltfAndExternalData`/`loadGltf` for real files with external refs.
- `GltfReaderResult` uses `optional` + string vectors (not `CesiumUtility::Result`) — check `.model` before use.
- KTX2 handling: without `ktx2TranscodeTargets` set, KTX2 textures are fully decompressed to raw pixels (memory-heavy); set transcode targets to get GPU-compressed formats.
- `clearDecodedDataUrls=true` mutates the model to save memory — don't expect `buffer.uri` to survive.

---

## 8. CesiumImage

**Purpose.** Image decoding/transcoding/manipulation, producing `ImageAsset`s shared across tilesets. (Moved out of `CesiumGltfReader`/`CesiumGltfContent` in v0.62.0 per CHANGES.md.) Depends on `CesiumUtility`, `CesiumAsync`.

- `ImageAsset.h` — `struct ImageAsset final : public CesiumUtility::SharedAsset<ImageAsset>`:
  ```cpp
  int32_t width = 0, height = 0;
  int32_t channels = 4;        // 1=grey, 2=grey+alpha, 3=RGB, 4=RGBA
  int32_t bytesPerChannel = 1;
  GpuCompressedPixelFormat compressedPixelFormat = GpuCompressedPixelFormat::NONE;
  std::vector<ImageAssetMipPosition> mipPositions; // byteOffset/byteSize per mip; empty => single image
  std::vector<std::byte> pixelData; // tightly packed, no row padding; stb-compatible layout when uncompressed
  ```
  Pixel data layout: `width*height*channels*bytesPerChannel` bytes, no padding. Channel order follows stb_image (RGB/RGBA, first byte = R).
- `ImageDecoder.h` — `class ImageDecoder` (all static):
  - `static ImageReaderResult readImage(span<const byte> data, const Ktx2TranscodeTargets& ktx2TranscodeTargets)` — stb_image for JPG/PNG/TGA/BMP/PSD/GIF/HDR/PIC; KTX2 transcoded to the requested GPU formats or fully decompressed if no targets.
  - `static std::optional<std::string> generateMipMaps(ImageAsset& image)` — no-op if mips exist or image is GPU-compressed; returns error string on failure.
  - `static bool unsafeResize(...)` — raw pointer resize without validation.
- `ImageManipulation.h` — `class ImageManipulation`: `static void unsafeBlitImage(...)`, `static bool blitImage(...)` (bounds-checked blit used by raster overlays), `static std::vector<std::byte> savePng(const ImageAsset&)` / `savePng(image, output)`.
- `Ktx2TranscodeTargets.h` — `struct Ktx2TranscodeTargets`: per-input-format target GPU pixel format (e.g. BC7/ETC2/ASTC), consumed by `GltfReaderOptions::ktx2TranscodeTargets`.

### Gotchas

- `ImageAsset` is a `SharedAsset` — don't `delete` it; hold an `IntrusivePointer<ImageAsset>`.
- `mipPositions` empty means "single image, no mips" — clients needing mips must call `generateMipMaps` (which refuses GPU-compressed images).
- `unsafeBlitImage`/`unsafeResize` are genuinely unsafe (no validation) — prefer the checked `blitImage`.

---

## 9. Cesium3DTiles / Cesium3DTilesReader / Cesium3DTilesWriter

**Purpose.** The 3D Tiles JSON data model (`tileset.json`, `subtree.json`), generated from the spec exactly like `CesiumGltf`. `Cesium3DTilesReader` parses them; `Cesium3DTilesWriter` serializes them.

### Cesium3DTiles (`Cesium3DTiles/generated/include/Cesium3DTiles/` + handwritten)

Generated structs (namespace `Cesium3DTiles`): `TilesetSpec` (root: `asset`, `geometricError`, `root` tile, `extensionsUsed/Required`, optional `schema`/`schemaUri`/`statistics`/`groups`/`metadata`, `getSizeBytes()` estimator), `Tile` (boundingVolume, `viewerRequestVolume?`, `geometricError`, `refine?` ("ADD"/"REPLACE"), `transform` (16 doubles, column-major), `content?`, `contents[]` (1.1 multiple contents), `metadata?`, `implicitTiling?`, `children[]`), `BoundingVolume` (region/box/sphere + extensions like S2/cylinder), `Content`, `ImplicitTiling` (subdivisionScheme QUADTREE/OCTREE, `subtreeLevels`, `availableLevels`, `subtrees.uri` template), `Subtree`/`Subtrees`, `BufferSpec`/`BufferView`, `Availability`, `Class`/`ClassProperty`/`Enum`/`EnumValue`/`Schema`, `PropertyTable`/`PropertyTableProperty`, `GroupMetadata`, `MetadataEntity`, `Statistics`/`ClassStatistics`/`PropertyStatistics`, `Extension*` (e.g. `Extension3dTilesBoundingVolumeS2`, `Extension3dTilesBoundingVolumeCylinder`, `Extension3dTilesEllipsoid`, `ExtensionTilesetMaxarContentGeoJson`).

Handwritten (`Cesium3DTiles/include/Cesium3DTiles/`):
- `Tileset.h` — `struct Tileset : public TilesetSpec` (verify exact inheritance/helpers in header).
- `Buffer.h`/`BufferCesium.h` — payload carriers analogous to glTF's (`cesiumData`).
- `MetadataQuery.h` — helpers to query `EXT_structural_metadata`-style tileset metadata (verify signatures in header).

### Cesium3DTilesReader

- `TilesetReader.h` — `class TilesetReader`: `readTileset(span, options)` → result with `optional<Tileset>` + errors/warnings (same shape as `GltfReaderResult`); `loadTileset(asyncSystem, url, headers, pAssetAccessor, options)` → `Future<...>`. Generated `*Reader` per struct (e.g. `TileReader.h`, `BoundingVolumeReader.h`) built on `CesiumJsonReader`.
- `SubtreeFileReader.h` — `class SubtreeFileReader`: reads binary `.subtree` files (`Subtree` JSON + buffers).

### Cesium3DTilesWriter

- `TilesetWriter.h` — `class TilesetWriter`: `getExtensions()` / `TilesetWriterResult writeTileset(const Tileset&, options)` (same result shape as `GltfWriterResult`: bytes + errors + warnings).
- `SubtreeWriter.h` — writes binary subtree files; `SchemaWriter.h`, `ConditionalContentWriter.h` for extensions.

### Gotchas

- `Tile::refine` is `std::optional<std::string>` — absent means "inherit from parent" per spec; don't default it to REPLACE yourself.
- `transform` is a 16-double **column-major** matrix; identity default. Implicit tiles compose ancestor transforms — see `TileTransform.h` in §10 for the accumulation helper.
- `contents` (plural, 3D Tiles 1.1) vs legacy `content` (singular): both may appear; the selection engine handles both.
- Like glTF, `generated/` is public but machine-written — never hand-edit.

---

## 10. Cesium3DTilesContent

**Purpose.** Converts legacy 3D Tiles tile formats (b3dm, i3dm, pnts, cmpt) and glTF-direct content into `CesiumGltf::Model`s; plus helpers for implicit tiling, bounding volumes, and subtree availability used by the selection engine.

Key headers (`Cesium3DTilesContent/include/Cesium3DTilesContent/`):

```cpp
struct GltfConverterResult {
  std::optional<CesiumGltf::Model> model;
  CesiumUtility::ErrorList errors; // (value + diagnostics pattern)
};

struct B3dmToGltfConverter {
  static CesiumAsync::Future<GltfConverterResult> convert(
      const std::span<const std::byte>& b3dmBinary,
      const CesiumAsync::AsyncSystem& asyncSystem,
      /* ...options incl. GltfConverterOptions... */);
};
// I3dmToGltfConverter, PntsToGltfConverter, CmptToGltfConverter: same convert() shape.
class BinaryToGltfConverter { /* shared base/helpers */ };
struct GltfConverters { /* dispatch by magic bytes */ };
void registerAllTileContentTypes(); // registers b3dm/i3dm/pnts/cmpt/glb handlers with the tile loading pipeline
```

- `GltfConverterUtility.h` — shared conversion helpers (feature tables, batch tables → `EXT_structural_metadata`).
- `ImplicitTilingUtilities.h` — `struct ImplicitTilingUtilities`: child tile ID computation, Morton indexing, template-URI substitution, subtree availability lookups.
- `SubtreeAvailability.h` — parses subtree buffer availability bitstreams.
- `TileBoundingVolumes.h` — converts spec `BoundingVolume` (region/box/sphere/cylinder/S2/ellipsoid) to selection-engine volumes; `TileTransform.h` — accumulates tile transforms down the hierarchy.

### Gotchas

- Converters are **async** (`Future<GltfConverterResult>`) because i3dm/pnts glTF embedding and image decoding can hop threads; pump the async system or `waitInMainThread()`.
- b3dm batch table / i3dm feature table become `EXT_structural_metadata` property tables in the output glTF — metadata survives, but in a different representation than the legacy JSON.
- `registerAllTileContentTypes()` must be called once before tileset loading if you rely on the default content-type dispatch (the `Tileset` content manager calls it internally in the standard setup — verify if you construct loaders manually).

---

## 11. CesiumGltfContent

**Purpose.** glTF post-processing utilities used by the tile pipeline (RTC, up-axis, skirts, buffer compaction, ray intersection).

`CesiumGltfContent/include/CesiumGltfContent/GltfUtilities.h` — `struct GltfUtilities`, all static:
```cpp
static glm::dmat4x4 applyRtcCenter(...);            // CESIUM_RTC extension -> node transform
static glm::dmat4x4 applyGltfUpAxisTransform(...);   // Y-up (glTF) -> Z-up (3D Tiles/ECEF)
static CesiumGeospatial::BoundingRegion computeBoundingRegion(...);
static void collapseToSingleBuffer(CesiumGltf::Model& gltf);
static void moveBufferContent(...);
static void removeUnusedTextures/Samplers/Images/Accessors/BufferViews/Buffers/Meshes/Materials(CesiumGltf::Model&);
static void compactBuffers(CesiumGltf::Model& gltf);
static void compactBuffer(CesiumGltf::Model& gltf, int32_t bufferIndex);
static IntersectResult intersectRayGltfModel(...);  // CPU ray-mesh intersection
```
- `SkirtMeshMetadata.h` — metadata for terrain skirt meshes (used by quantized-mesh upsampling).

### Gotchas

- `applyGltfUpAxisTransform` is the Z-up fix-up — tile content pipelines apply it so tiles land correctly in ECEF; double-applying flips models.
- `removeUnused*`/`compactBuffers` mutate the model in place and rewrite indices — don't hold stale accessor/buffer indices across the call.

---

## 12. Cesium3DTilesSelection

**Purpose.** The tileset traversal/selection engine: loads `tileset.json`, walks the tile hierarchy each frame, decides which tiles to render based on screen-space error (SSE), frustum/occlusion/fog culling, and streams tile content through the renderer's resource-preparation hooks. This is the module host apps interact with most. Depends on nearly everything above.

### 12.1 `Tileset` — the entry point

`Cesium3DTilesSelection/include/Cesium3DTilesSelection/Tileset.h` — `class Tileset final` (non-copyable).

```cpp
Tileset(const TilesetExternals& externals, const std::string& url,
        const TilesetOptions& options = {});                       // from tileset.json URL
Tileset(const TilesetExternals& externals, int64_t ionAssetID,
        const std::string& ionAccessToken,
        const TilesetOptions& options = {},
        const std::string& ionAssetEndpointUrl = "https://api.cesium.com/"); // from Cesium ion
Tileset(const TilesetExternals& externals,
        std::unique_ptr<TilesetContentLoader>&& pCustomLoader,
        std::unique_ptr<Tile>&& pRootTile,
        const TilesetOptions& options = {});                       // fully custom loader
Tileset(const TilesetExternals& externals,
        TilesetContentLoaderFactory&& loaderFactory,
        const TilesetOptions& options = {});                       // loader factory
~Tileset() noexcept; // unloads tile content synchronously where possible; remainder
                     // unloads async — observe getAsyncDestructionCompleteEvent()

CesiumAsync::SharedFuture<void>& getAsyncDestructionCompleteEvent();
CesiumAsync::SharedFuture<void>& getRootTileAvailableEvent(); // resolves when root tile *metadata* is available (content may still be loading)
const Tile* getRootTile() const noexcept;  // may be nullptr before root-tile event
Tile* getRootTile() noexcept;
TilesetViewGroup& getDefaultViewGroup();

// Per-frame update (call updateViewGroup, then loadTiles, once per render frame):
const ViewUpdateResult& updateViewGroup(TilesetViewGroup& viewGroup,
                                        const std::vector<ViewState>& frustums,
                                        float deltaTime = 0.0f);
const ViewUpdateResult& updateViewGroupOffline(TilesetViewGroup& viewGroup,
                                               const std::vector<ViewState>& frustums); // blocking, for movie capture
void loadTiles(); // starts/continues async loads for all view groups; call frequently
// NOTE: the older updateView(fr ustums, deltaTime) is [[deprecated]] in favor of
// updateViewGroup(getDefaultViewGroup(), ...) + loadTiles().

int32_t getNumberOfTilesLoaded() const;
float computeLoadProgress() noexcept;      // default view group's load %
int64_t getTotalDataBytes() const noexcept;
void forEachLoadedTile(const std::function<void(const Tile&)>& callback) const;
LoadedConstTileEnumerator loadedTiles() const;

const TilesetMetadata* getMetadata(const Tile* pTile = nullptr) const; // sync; nullptr if not loaded yet
CesiumAsync::Future<const TilesetMetadata*> loadMetadata();            // async; resolves after root tile + schemaUri
CesiumAsync::Future<SampleHeightResult> sampleHeightMostDetailed(
    const std::vector<CesiumGeospatial::Cartographic>& positions);
// sampleHeight requires updateView to be pumped, else the future never resolves.
// Heights are meters ABOVE THE ELLIPSOID.

void registerLoadRequester(TileLoadRequester& requester);
bool waitForAllLoadsToComplete(double maximumWaitTimeInMilliseconds); // main thread only, blocks
std::optional<CesiumUtility::Credit> getUserCredit() const noexcept;
const std::vector<CesiumUtility::Credit>& getTilesetCredits() const noexcept;
void setShowCreditsOnScreen(bool showCreditsOnScreen) noexcept;
```

### 12.2 `TilesetExternals` and `TilesetOptions`

`TilesetExternals.h` — `struct TilesetExternals` (**"Not supposed to be used by clients"** — it is filled by whoever constructs the `Tileset`, typically the engine integration):
```cpp
std::shared_ptr<CesiumAsync::IAssetAccessor> pAssetAccessor;
std::shared_ptr<IPrepareRendererResources> pPrepareRendererResources;
CesiumAsync::AsyncSystem asyncSystem;   // Tileset::updateViewGroup dispatches its main-thread tasks
std::shared_ptr<CesiumUtility::CreditSystem> pCreditSystem;
std::shared_ptr<spdlog::logger> pLogger = spdlog::default_logger();
std::shared_ptr<TileOcclusionRendererProxyPool> pTileOcclusionProxyPool = nullptr; // null => no occlusion culling
CesiumUtility::IntrusivePointer<TilesetSharedAssetSystem> pSharedAssetSystem = TilesetSharedAssetSystem::getDefault();
std::shared_ptr<GltfModifier> pGltfModifier = {};
```
`TilesetOptions.h` — `struct TilesetOptions` (selected fields with defaults):
```cpp
double maximumScreenSpaceError = 16.0;
uint32_t maximumSimultaneousTileLoads = 20;
bool preloadAncestors = true, preloadSiblings = true;
uint32_t loadingDescendantLimit = 20;
bool forbidHoles = false;
bool enableFrustumCulling = true, enableOcclusionCulling = true, enableFogCulling = true;
bool enforceCulledScreenSpaceError = true;
double culledScreenSpaceError = 64.0;
int64_t maximumCachedBytes = 512LL * 1024 * 1024;   // tile cache budget
bool renderTilesUnderCamera = true;
bool enableLodTransitionPeriod = false;             // fading; see ViewUpdateResult::tilesFadingOut
float lodTransitionLength = 1.0f;
bool kickDescendantsWhileFadingIn = true;
double mainThreadLoadingTimeLimit = 0.0;            // 0 = no limit
double tileCacheUnloadTimeLimit = 0.0;
std::optional<std::string> credit;                   // user credit string
std::vector<std::shared_ptr<ITileExcluder>> excluders;
CesiumGeospatial::Ellipsoid ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;
TilesetContentOptions contentOptions;
std::any rendererOptions;                            // passed through to IPrepareRendererResources
std::vector<CesiumAsync::IAssetAccessor::THeader> requestHeaders;
struct FogDensityAtHeight { double cameraHeight; double fogDensity; };
std::vector<FogDensityAtHeight> fogDensityTable = {...}; // default table in header
```

### 12.3 `ViewState` — the camera

`ViewState.h` — `class ViewState final`:
```cpp
ViewState(const glm::dvec3& position, const glm::dvec3& direction, const glm::dvec3& up,
          const glm::dvec2& viewportSize,
          double horizontalFieldOfView, double verticalFieldOfView,
          const CesiumGeospatial::Ellipsoid& ellipsoid = WGS84); // symmetric perspective
ViewState(const glm::dmat4& viewMatrix, const glm::dmat4& projectionMatrix,
          const glm::dvec2& viewportSize, const Ellipsoid& ellipsoid = WGS84); // general projection
ViewState(position, direction, up, viewportSize, left, right, bottom, top, ellipsoid); // orthographic
ViewState(const BoundingVolume& boundingVolume, double geometricErrorThreshold,
          const Ellipsoid& ellipsoid = WGS84); // area paging (v0.64.0): region selection at fixed LOD, no SSE
// getters: getPosition(), getDirection(), getUp(), getViewportSize(), getPositionCartographic(), ...
```
Position/direction/up are **ECEF (Z-up, meters)**. FOVs are **radians**. (`ViewState::create(...)` static is deprecated — use constructors.)

### 12.4 `Tile`, `TileContent`, `TileID`

- `Tile.h` — `class Tile`: reference-counted (`addReference`/`releaseReference`, `getReferenceCount()`; `using Pointer = IntrusivePointer<Tile>`). Key API: `getParent()`, `getChildren()` (span), `getBoundingVolume()`/`setBoundingVolume()`, `getContentBoundingVolume()`, `getViewerRequestVolume()`, `getGeometricError()`, `getRefine()`/`setRefine()` (`TileRefine.h`: Add/Replace), `getTransform()`/`setTransform()` (dmat4), `getTileID()`/`setTileID()`, `getContent()` (`const TileContent&` / `TileContent&`), `isRenderable()/isRenderContent()/isExternalContent()/isEmptyContent()`, `getLoader()`, `getState() → TileLoadState`, `computeByteSize()`, `getMappedRasterTiles()`.
  - `enum class TileLoadState { Unloading=-2, FailedTemporarily=-1, Unloaded=0, ContentLoading=1, ContentLoaded=2, Done=3, Failed=4 }`.
  - `DebugTileStateDatabase.h` — `Tile::addEntry(...)` hooks for debugging tile state transitions.
- `TileContent.h` — `class TileContent`: variant over `TileUnknownContent`, `TileEmptyContent`, `TileExternalContent` (points at another tileset.json), `TileRenderContent` (holds the glTF `Model` + `pRenderResources` void* from `IPrepareRendererResources`).
- `TileID.h` — `typedef std::variant<std::string, CesiumGeometry::QuadtreeTileID, CesiumGeometry::OctreeTileID, CesiumGeometry::UpsampledQuadtreeNode> TileID`; `TileIdUtilities::createTileIdString()` (unspecified format, for logs), `isLoadable()`.
- `BoundingVolume.h` (selection) — `typedef std::variant<BoundingSphere, OrientedBoundingBox, BoundingRegion, BoundingRegionWithLooseFittingHeights, S2CellBoundingVolume, BoundingCylinderRegion> BoundingVolume`.

### 12.5 `ViewUpdateResult`, `TilesetViewGroup`

`ViewUpdateResult.h` — `class ViewUpdateResult final`:
```cpp
std::vector<Tile::ConstPointer> tilesToRenderThisFrame; // render these
std::vector<double> tileScreenSpaceErrorThisFrame;      // parallel SSE array
std::unordered_set<Tile::ConstPointer> tilesFadingOut;   // hide when fade % hits 0 (LOD transitions)
int32_t workerThreadTileLoadQueueLength, mainThreadTileLoadQueueLength;
uint32_t tilesVisited, culledTilesVisited, tilesCulled, tilesOccluded,
         tilesWaitingForOcclusionResults, tilesKicked, maxDepthVisited;
int32_t frameNumber; // incremented per updateViewGroup call
```
The returned reference is valid only until the next `updateViewGroup` call or tileset destruction.
`TilesetViewGroup.h` — per-view-group state (frustums, previous load progress); create one per independent view (main camera, shadow camera, minimap…), reuse across frames.

### 12.6 Loading pipeline interfaces

- `TilesetContentLoader.h` — `class TilesetContentLoader`: `virtual Future<TileLoadResult> loadTileContent(const TileLoadInput&)`; `virtual TileChildrenResult createTileChildren(...)`; `virtual ITilesetHeightSampler* getHeightSampler()`; `setExternalSchema`/`getExternalSchema`; `setOwnerOfNestedLoaders`. Implement to support custom tile sources. `TilesetContentLoaderFactory.h` / `TilesetContentLoaderResult.h` / `TilesetContentOptions.h` accompany it.
- `TileLoadResult.h` — `struct TileLoadResult { TileLoadResultState state; TileContentKind contentKind; std::optional<BoundingVolume> updatedBoundingVolume, updatedContentBoundingVolume; ... pAssetAccessor; pCompletedRequest; }` with `enum class TileLoadResultState { Success, Failed, RetryLater }` — **on Failed/RetryLater none of the fields are applied**.
- `IPrepareRendererResources.h` — `class IPrepareRendererResources : public RasterOverlays::IPrepareRasterOverlayRendererResources` (**"It is not supposed to be used directly by clients"** — implement for your engine):
  ```cpp
  virtual Future<TileLoadResultAndRenderResources> prepareInLoadThread(
      const AsyncSystem&, TileLoadResult&&, const glm::dmat4& transform,
      const std::any& rendererOptions) = 0;   // worker thread
  virtual void* prepareInMainThread(Tile& tile, void* pLoadThreadResult) = 0; // updateView thread
  virtual void free(Tile&, void* pLoadThreadResult, void* pMainThreadResult) noexcept = 0;
  virtual void attachRasterInMainThread(const Tile&, int32_t overlayTextureCoordinateID,
      const RasterOverlayTile&, void* pMainThreadRendererResources,
      const glm::dvec2& translation, const glm::dvec2& scale) = 0;
  virtual void detachRasterInMainThread(...) noexcept = 0;
  ```
  The `void*` render resources are opaque to cesium-native; `TileRenderContent` carries them back to the renderer.
- `ITileExcluder.h` — `class ITileExcluder { virtual bool shouldExclude(const Tile&) const noexcept = 0; virtual void startNewFrame() noexcept {} }` — plug into `TilesetOptions::excluders` (e.g. `RasterizedPolygonsTileExcluder.h`).
- `GltfModifier.h` — `class GltfModifier : private TileLoadRequester`: post-load glTF mesh modification hook (split/merge), async via `Future<std::optional<GltfModifierOutput>>`.
- `ITilesetHeightSampler.h`, `TilesetHeightQuery` (internal), `TileOcclusionRendererProxy.h` (occlusion query pool), `TileSelectionState.h`, `TileUnloadQueue.h`, `RasterOverlayCollection.h` (`add`/`remove` overlays at runtime), `LoadedTileEnumerator.h`.
- `CesiumIonTilesetContentLoaderFactory.h` — builds a loader from a Cesium ion asset ID + token.

### Typical usage (from `Cesium3DTilesSelection/test/TestTilesetSelection.cpp`)

```cpp
#include <Cesium3DTilesSelection/Tileset.h>
TilesetExternals externals{
    .pAssetAccessor = std::make_shared<MyAssetAccessor>(),
    .pPrepareRendererResources = std::make_shared<MyPrepareRendererResources>(),
    .asyncSystem = CesiumAsync::AsyncSystem(std::make_shared<MyTaskProcessor>()),
    .pCreditSystem = std::make_shared<CesiumUtility::CreditSystem>(),
};
TilesetOptions options;
options.maximumScreenSpaceError = 16.0;
auto pTileset = std::make_unique<Tileset>(externals, "https://example.com/tileset.json", options);

// once per frame:
ViewState viewState(camPos, camDir, camUp, viewportSize, hFov, vFov);
const ViewUpdateResult& result =
    pTileset->updateViewGroup(pTileset->getDefaultViewGroup(), {viewState}, deltaTime);
pTileset->loadTiles();
for (const auto& pTile : result.tilesToRenderThisFrame) { /* render pTile->getContent() */ }
```

### Gotchas — Cesium3DTilesSelection

- **You must call both `updateViewGroup` AND `loadTiles()` every frame.** `updateViewGroup` only *selects*; `loadTiles()` pumps the load queues. Without `loadTiles()`, nothing new ever loads (and `sampleHeightMostDetailed` futures never resolve).
- `ViewUpdateResult` references are **frame-transient** — copy what you need (the `Tile::ConstPointer`s keep tiles alive).
- Tiles are reference-counted and can be unloaded at any time when not referenced; holding `Tile::Pointer`/`ConstPointer` across frames is the way to keep content alive.
- `Tileset` destructor unloads asynchronously — if your renderer frees GPU resources in `IPrepareRendererResources::free`, either wait on `getAsyncDestructionCompleteEvent()` or ensure `free` is safe to call after the `Tileset` is gone.
- `maximumCachedBytes` (default 512 MB) bounds the tile cache; exceeding it unloads least-recently-used tiles even if still referenced by nothing.
- SSE FOVs are radians; `maximumScreenSpaceError` default 16.0 matches cesium.js.
- `ViewState` positions are ECEF meters — convert with `Ellipsoid::cartographicToCartesian` (and remember `fromDegrees`).
- `getRootTile()` returns nullptr until `getRootTileAvailableEvent()` resolves; even then, tile *content* loads later (`TileLoadState::Done`).

---

## 13. CesiumRasterOverlays

**Purpose.** Drape imagery (or other raster data) over 3D tiles: overlay sources fetch raster tiles asynchronously, and the selection engine projects them onto geometry tiles via generated texture coordinates. Depends on `CesiumAsync`, `CesiumGeospatial`, `CesiumImage`, `CesiumUtility`, `CesiumVectorData` (for polygon overlays).

### Core classes (`CesiumRasterOverlays/include/CesiumRasterOverlays/`)

- `RasterOverlay.h` — `class RasterOverlay` (base; reference-counted via `IntrusivePointer`):
  ```cpp
  const std::string& getName() const noexcept;
  RasterOverlayOptions& getOptions() noexcept;
  CesiumUtility::IntrusivePointer<ActivatedRasterOverlay> activate(
      const RasterOverlayExternals& externals /* asyncSystem, pAssetAccessor, ... — verify */,
      const CesiumUtility::IntrusivePointer<RasterOverlay>& pSelf) /* approx */;
  CesiumUtility::IntrusivePointer<RasterOverlayTileProvider> createPlaceholder(...);
  virtual CesiumAsync::Future<CreateTileProviderResult> createTileProvider(
      const CreateRasterOverlayTileProviderParameters& params) = 0; // implemented per source
  ```
  `RasterOverlayOptions.h` (verify filename) carries max cache sizes etc.
- `RasterOverlayTileProvider.h` — per-overlay tile pyramid: fetches/decodes raster images into `RasterOverlayTile`s, manages its own cache and credits.
- `RasterOverlayTile.h` — one raster image tile: states (Loading/Loaded/Failed/Empty/Placeholder), `getImage()` → `ImageAsset`, rectangle, credits.
- `ActivatedRasterOverlay.h` — runtime binding of an overlay to a tileset (created by `RasterOverlay::activate`, added to `RasterOverlayCollection`).
- `RasterOverlayCollection.h` (`Cesium3DTilesSelection`) — `add(IntrusivePointer<RasterOverlay>)`, `remove(...)`, iteration; owned by `Tileset::getOverlays()`.
- `RasterMappedTo3DTile.h` — how a raster tile maps onto a geometry tile (texture-coordinate ID, translation/scale) — consumed by `IPrepareRendererResources::attachRasterInMainThread` as `uv = overlayUV * scale + translation`.
- `IPrepareRasterOverlayRendererResources.h`:
  ```cpp
  virtual void* prepareRasterInLoadThread(...);
  virtual void* prepareRasterInMainThread(...);
  virtual void freeRaster(...) noexcept;
  ```
- `QuadtreeRasterOverlayTileProvider.h` — base for quadtree-based imagery providers.
- `RasterOverlayUtilities.h`, `RasterOverlayDetails.h`, `RasterOverlayExternals.h`, `RasterOverlayTileProvider` helpers.

### Concrete overlays

| Class | Source |
|---|---|
| `UrlTemplateRasterOverlay.h` | `UrlTemplateRasterOverlay(name, url, headers={}, urlTemplateOptions={}, overlayOptions={})` — `{x}/{y}/{level}` URL templates (e.g. OSM-style tile servers) |
| `TileMapServiceRasterOverlay.h` | TMS endpoints |
| `WebMapServiceRasterOverlay.h` | WMS `GetMap` |
| `WebMapTileServiceRasterOverlay.h` | WMTS |
| `BingMapsRasterOverlay.h` | Bing Maps (requires API key) |
| `GoogleMapTilesRasterOverlay.h` | Google Maps Platform 2D Map Tiles API (requires key) |
| `AzureMapsRasterOverlay.h` | Azure Maps (requires key/credential) |
| `IonRasterOverlay.h` | Cesium ion imagery asset (asset ID + token) |
| `RasterizedPolygonsOverlay.h` | Rasterizes cartographic polygons on the CPU into overlay tiles (uses `CesiumVectorData`) |
| `DebugColorizeTilesRasterOverlay.h` | Debug: colors raster tiles by ID |
| `ITwinCesiumCuratedContentRasterOverlay.h` | iTwin curated content |

(Constructor signatures for keyed overlays — verify in each header; they take key/token strings plus options structs.)

### Typical usage

```cpp
#include <CesiumRasterOverlays/UrlTemplateRasterOverlay.h>
auto pOverlay = CesiumUtility::IntrusivePointer<CesiumRasterOverlays::RasterOverlay>(
    new CesiumRasterOverlays::UrlTemplateRasterOverlay(
        "OpenStreetMap",
        "https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
pTileset->getOverlays().add(pOverlay); // activation happens internally
```

### Gotchas

- Overlays are added to the tileset's `RasterOverlayCollection`; the tileset activates them. Don't call `createTileProvider` yourself unless you're building custom plumbing.
- Raster tiles arrive on their own async cadence — geometry renders first, imagery pops in later. `attachRasterInMainThread` is where your renderer binds the texture.
- `overlayTextureCoordinateID` selects which UV set the overlay uses; mismatched IDs = imagery sampled with wrong coordinates.
- Keyed providers (Bing/Google/Azure/ion) need valid credentials; failures surface as raster-tile load errors, not tileset failures.

---

## 14. CesiumVectorData / CesiumVectorOverlays

**Purpose.** Parse GeoJSON into vector geometry, convert it to glTF or rasterize it to images for use as raster overlays / clipping polygons.

### CesiumVectorData (`CesiumVectorData/include/CesiumVectorData/`)

- `GeoJsonDocument.h` — `class GeoJsonDocument`:
  ```cpp
  static CesiumUtility::Result<GeoJsonDocument> fromGeoJson(const std::span<const std::byte>& geoJson);
  static CesiumAsync::Future<CesiumUtility::Result<GeoJsonDocument>> fromUrl(
      const AsyncSystem&, const std::shared_ptr<IAssetAccessor>&, const std::string& url, ...);
  static CesiumUtility::Result<GeoJsonObject> ...; // object-level parse helpers
  ```
  `struct VectorDocumentAttribution` — attribution from the document.
- `GeoJsonObject.h` / `GeoJsonObjectTypes.h` — variant over Point/MultiPoint/LineString/MultiLineString/Polygon/MultiPolygon/GeometryCollection/Feature/FeatureCollection.
- `VectorStyle.h` — `struct VectorStyle`: colors, line widths, point sizes for rasterization.
- `VectorRasterizer.h` — `class VectorRasterizer`: rasterizes vector geometry into an `ImageAsset` given a `VectorStyle` and geographic rectangle.
- `GltfConverter.h` — `class GltfConverter`: `static ConverterResult convert(...)` (vector → glTF model), `static ConvertSchemaResult convertSchema(...)`.

### CesiumVectorOverlays (`CesiumVectorOverlays/include/CesiumVectorOverlays/`)

- `GeoJsonDocumentRasterOverlay.h` — a `RasterOverlay` that rasterizes a `GeoJsonDocument` on the fly (points/polylines/polygons styled by `VectorStyle`).
- `VectorTilesRasterOverlay.h` — raster overlay sourced from vector tiles.

### Gotchas

- `GeoJsonDocument::fromGeoJson` returns `Result<GeoJsonDocument>` — check `.value` before use.
- Rasterizing large polygon sets per tile is CPU-heavy; `RasterizedPolygonsOverlay` caches per raster tile, but style changes invalidate.

---

## 15. CesiumQuantizedMeshTerrain

**Purpose.** Parse and generate quantized-mesh terrain tiles (the format behind Cesium ion terrain / `layer.json` terrain endpoints). Converts heightmap meshes into glTF for the selection engine.

`CesiumQuantizedMeshTerrain/include/CesiumQuantizedMeshTerrain/`:
- `QuantizedMeshLoader.h` — `class QuantizedMeshLoader final`:
  ```cpp
  struct QuantizedMeshLoadResult { /* glTF model + errors — verify fields */ };
  static QuantizedMeshLoadResult load(...);                    // parse a .terrain tile
  static QuantizedMeshMetadataResult loadMetadata(...);        // layer.json
  static QuantizedMeshMetadataResult loadAvailabilityRectangles(...);
  ```
- `Layer.h` / `LayerWriter.h` — `layer.json` model + serialization (`LayerSpec.h`/`LayerReader.h` generated).
- `AvailabilityRectangle.h` — available-terrain rectangles.

Terrain tile loading in a tileset is normally done through the loader created for a quantized-mesh endpoint (see `Cesium3DTilesSelection/test/TestLayerJsonTerrainLoader.cpp` for the wiring pattern), not by calling `QuantizedMeshLoader` directly.

### Gotchas

- Quantized-mesh uses its own vertex compression (zigzag + delta); the loader dequantizes to float positions.
- `loadMetadata` reads `layer.json`, which defines projection, available levels, and extensions — required before any tile URLs can be formed.

---

## 16. CesiumIonClient

**Purpose.** Typed REST client for the Cesium ion API (auth, assets, tokens, geocoder, defaults). All calls are async (`Future<Response<T>>`).

`CesiumIonClient/include/CesiumIonClient/`:
- `Connection.h` — `class Connection`:
  ```cpp
  static CesiumAsync::Future<CesiumUtility::Result<Connection>> authorize(
      const AsyncSystem&, const std::shared_ptr<IAssetAccessor>&,
      const std::string& friendlyApplicationName,
      const std::vector<std::string>& scopes, ...); // OAuth2 browser flow
  static CesiumAsync::Future<CesiumUtility::Result<Connection>> ...; // token-based ctor (verify overloads)
  const AsyncSystem& getAsyncSystem() const noexcept;
  const std::string& getAccessToken() const noexcept;
  const std::string& getApiUrl() const noexcept;
  CesiumAsync::Future<Response<Profile>> me() const;
  CesiumAsync::Future<Response<Defaults>> defaults() const;
  CesiumAsync::Future<Response<Assets>> assets() const;
  CesiumAsync::Future<Response<Asset>> asset(int64_t assetID) const;
  CesiumAsync::Future<Response<Token>> token(const std::string& tokenID) const;
  CesiumAsync::Future<Response<Token>> createToken(...);
  CesiumAsync::Future<Response<NoValue>> modifyToken(...);
  CesiumAsync::Future<Response<TokenList>> tokens(const std::string& url) const;
  CesiumAsync::Future<Response<GeocoderResult>> geocode(...);
  static std::optional<std::string> getIdFromToken(const std::string& token);
  ```
- `Response.h` — `template <typename T> struct Response` (HTTP status + value + error message pattern; verify fields).
- `Assets.h`/`ApplicationData.h`/`Defaults.h`/`Geocoder.h`/`Profile.h`/`Token.h`/`TokenList.h`/`LoginToken.h` — data models.

### Gotchas

- `authorize` runs an OAuth2 flow that needs user interaction (browser) — on headless/server apps, create a token in the ion dashboard and construct the `Connection` from the token instead.
- `Response<T>` is not `Result<T>` — check the HTTP status/error fields per the header.

---

## 17. CesiumITwinClient

**Purpose.** Typed REST client for the Bentley iTwin Platform APIs (used by the iTwin reality-data and curated-content tileset loaders).

`CesiumITwinClient/include/CesiumITwinClient/`:
- `Connection.h` — `class Connection`:
  ```cpp
  static CesiumAsync::Future<CesiumUtility::Result<Connection>> authorize(...); // OAuth2 PKCE via CesiumClientCommon
  CesiumAsync::Future<CesiumUtility::Result<UserProfile>> me();
  CesiumAsync::Future<CesiumUtility::Result<std::string>> ensureValidToken(); // refreshes if needed
  const AuthenticationToken& getAuthenticationToken() const;
  void setAuthenticationToken(const AuthenticationToken&);
  const std::optional<std::string>& getRefreshToken() const;
  ```
- `IModel.h`, `IModelMeshExport.h` (+ `IModelMeshExportContentLoaderFactory.h` in Selection) — iModel mesh export jobs → 3D Tiles.
- `ITwinRealityData.h` (+ `ITwinRealityDataContentLoaderFactory.h` in Selection) — reality data → tilesets.
- `CesiumCuratedContent.h` (+ `ITwinCesiumCuratedContentLoaderFactory.h` / `ITwinCesiumCuratedContentRasterOverlay.h`) — Cesium curated content on iTwin.
- `GeospatialFeatureCollection.h`, `PagedList.h`, `Profile.h`, `ITwin.h`.

### Gotchas

- Tokens expire — `ensureValidToken()` before making calls in long-lived sessions; the selection loaders handle this internally.

---

## 18. CesiumClientCommon

**Purpose.** Shared HTTP/auth primitives used by the ion and iTwin clients.

`CesiumClientCommon/include/CesiumClientCommon/`:
- `OAuth2PKCE.h` — `class OAuth2PKCE`: `struct OAuth2TokenResponse`, `struct OAuth2ClientOptions`; statics to `authorize` (PKCE code flow), exchange codes, and refresh tokens → `Future<Result<OAuth2TokenResponse>>`.
- `JwtTokenUtility.h` — `static Result<rapidjson::Document> parseTokenClaims(...)`-style JWT claim decoding (verify exact name/signature in header).
- `ErrorResponse.h` — common API error body parsing.
- `fillWithRandomBytes.h` — CSPRNG helper for PKCE verifiers.

**Internal note:** consumers normally don't touch this module directly; they go through `CesiumIonClient::Connection::authorize` or `CesiumITwinClient::Connection::authorize`.

---

## 19. CesiumCurl

**Purpose.** The production `IAssetAccessor` implementation based on libcurl.

`CesiumCurl/include/CesiumCurl/CurlAssetAccessor.h`:
```cpp
struct CurlAssetAccessorOptions { /* verify fields: verbose logging, etc. */ };
class CurlAssetAccessor : public CesiumAsync::IAssetAccessor {
public:
  CurlAssetAccessor(const CurlAssetAccessorOptions& options = {});
  // implements get()/request()/tick()
private:
  class CurlHandle; // internal
};
```
Typical wiring: `externals.pAssetAccessor = std::make_shared<CesiumCurl::CurlAssetAccessor>();` — often wrapped in `CachingAssetAccessor` (SQLite) and/or `GunzipAssetAccessor`.

### Gotchas

- Requires libcurl at link time; on some platforms you must call curl's global init yourself (verify for your platform — the header/implementation notes say; check `CurlAssetAccessor.cpp`).
- Not usable from WinRT/UWP-style sandboxes; provide your own `IAssetAccessor` there.

---

## 20. Global Gotchas

Cross-module pitfalls that are easy to get wrong, collected from header documentation across the library.

### Ownership & lifetime
- **`IntrusivePointer<T>` is the ownership currency** (`CesiumUtility/include/CesiumUtility/IntrusivePointer.h`). `Tile::Pointer`, `RasterOverlay` handles, `ImageAsset`s, `TilesetSharedAssetSystem` are all reference-counted. Raw pointers/references borrowed from them dangle when the last owner drops — notably `const ViewUpdateResult&` from `updateViewGroup` and `const Tile&` from `forEachLoadedTile`.
- **`AsyncSystem` must outlive all futures** (`CesiumAsync/include/CesiumAsync/AsyncSystem.h`). The safe default is a global/static instance.
- **`Tileset` destruction is partially asynchronous.** GPU resources freed in `IPrepareRendererResources::free` may be released after the destructor returns; use `getAsyncDestructionCompleteEvent()` if ordering matters.
- `SharedAsset` (e.g. `ImageAsset`) invalidation: `SharedAssetDepot` can invalidate assets; don't cache raw pointers to shared assets across depot resets.

### Threading & async
- **Main-thread continuations only run when pumped.** `Tileset::updateViewGroup` pumps internally via `dispatchMainThreadTasks`, but any standalone `Future` chain using `thenInMainThread` needs manual pumping or `waitInMainThread()`. `Future::wait()` on the main thread deadlocks.
- `thenInWorkerThread` runs the continuation on the `ITaskProcessor` background thread — the host app's `ITaskProcessor` implementation determines actual parallelism. `thenImmediately` runs in the resolving thread: keep it trivial and thread-safe.
- `ITaskProcessor` and `TilesetExternals` are documented **"Not supposed to be used by clients"** — they are implemented by the engine integration, not called by app logic. Same for `IPrepareRendererResources` (implement, don't invoke).
- Rejections are exceptions; `Result<T>` values are diagnostics. Don't mix the patterns: `Future<Result<T>>` means "transport failure → exception, parse failure → Result with errors".

### Optionals & generated models
- Generated structs use `std::optional` for spec-optional fields **and** plain members with spec defaults otherwise. Absence is meaningful (e.g. `Tile::refine` unset = inherit; `Model::scene = -1` = no default scene). Don't "fill in" defaults that change semantics.
- `extras` (`JsonValue`) preserves unknown JSON — forward-compatible parsing is built in; don't reject files for having extra keys.

### Coordinates
- glTF = Y-up, meters-or-whatever-the-asset-uses. 3D Tiles/ECEF = Z-up, meters. `Transforms::Y_UP_TO_Z_UP` and `GltfUtilities::applyGltfUpAxisTransform` exist for the conversion — apply exactly once.
- `Cartographic`: radians + meters. `Ellipsoid::WGS84` radii in meters. `Rectangle`/`GlobeRectangle`: radians. FOVs: radians.
- `sampleHeightMostDetailed` returns height **above the ellipsoid**, not MSL.

### Error handling
- Check `Result<T>::value` / `GltfReaderResult::model` / `TileLoadResult.state` before touching payloads. Warnings ≠ failure; `hasErrors()` / `errors.empty()` is the failure test.
- `TileLoadResultState::Failed`/`RetryLater` discard **all** fields — don't partially apply.
- `AccessorViewStatus` must be checked; an invalid view is not dereferenceable.

### Internal / do-not-use-directly
- Anything under `Impl/` or `detail/` directories, `CesiumImpl` namespace members (e.g. `CesiumImpl::ContinuationFutureType_t`, schedulers), and classes whose docs say "Not supposed to be used by clients" (`ITaskProcessor`, `TilesetExternals`, `IPrepareRendererResources` as a *caller*).
- `generated/` headers are public but machine-generated — read them, don't edit them; regenerate via `tools/generate-classes`.
- Test helpers (`CesiumNativeTests/include/...`: `SimpleAssetAccessor`, `SimpleTaskProcessor`, `waitForFuture.h`, `readFile.h`) are **not shipped** as a library — copy the pattern, don't link them.

### Build & linkage notes (from repo layout)
- Each module is its own CMake target (`CesiumAsync`, `CesiumGltf`, …); link only what you use. `Library.h` per module defines the `CESIUM<MODULE>_API` export macro.
- Public headers assume rapidjson, glm, spdlog, and (for networking) libcurl are available — they appear in public signatures (`HttpHeaders`, `glm::dvec3`, `spdlog::logger`).

---

## Areas not fully verified

The following were identified but not exhaustively read; signatures/behavior should be confirmed in the cited headers before relying on them:

- `CesiumGltf::Model` handwritten helpers (`CesiumGltf/include/CesiumGltf/Model.h`) — beyond `ModelSpec` fields.
- `CesiumUtility::JsonValue` accessor API (`CesiumUtility/include/CesiumUtility/JsonValue.h`).
- `CesiumUtility::Math` contents (`CesiumUtility/include/CesiumUtility/Math.h`).
- `CesiumIonClient::Response<T>` field layout (`CesiumIonClient/include/CesiumIonClient/Response.h`).
- `CesiumClientCommon::JwtTokenUtility` exact function names (`CesiumClientCommon/include/CesiumClientCommon/JwtTokenUtility.h`).
- `CesiumQuantizedMeshTerrain::QuantizedMeshLoadResult` / `QuantizedMeshMetadataResult` fields (`CesiumQuantizedMeshTerrain/include/CesiumQuantizedMeshTerrain/QuantizedMeshLoader.h`).
- `Cesium3DTiles::MetadataQuery` helpers (`Cesium3DTiles/include/Cesium3DTiles/MetadataQuery.h`).
- Exact `RasterOverlay::activate` / `createPlaceholder` signatures (`CesiumRasterOverlays/include/CesiumRasterOverlays/RasterOverlay.h`).
- `CesiumCurl` global-init requirements (`CesiumCurl/src/CurlAssetAccessor.cpp` — not a header).
- `CesiumGltfContent::GltfUtilities::intersectRayGltfModel` / `IntersectResult` exact shape.
- Behavior-level claims (e.g. cache eviction policy details, exact SSE formula) were taken from header doc comments only; the implementation `.cpp` files were not read.

---

*Document generated 2026-10-01 from cesium-native v0.64.0 source at `~/workspace/3dtiles-renderer/build/linux/_deps/cesium_native-src/`. For behavior beyond headers, see the module `test/` directories (doctest cases) and https://cesium.com/learn/cesium-native/.*
