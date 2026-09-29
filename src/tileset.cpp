// tiles_renderer SDK — 3D Tiles tileset integration (P3).
//
// Loads a tileset.json via cesium-native, selects tiles per frame with
// updateViewGroup, and converts each tile's glb content into Filament
// renderables with gltfio (+ ubershader materials).

#include "tileset_internal.h"

#include <unordered_set>

#ifdef TILES_WITH_CESIUM_NATIVE
#include <Cesium3DTilesSelection/IPrepareRendererResources.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/TileContent.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/TilesetOptions.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/IAssetResponse.h>
#include <CesiumAsync/ThreadPool.h>
#include <CesiumUtility/CreditSystem.h>
#endif

#ifdef TILES_WITH_FILAMENT
#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/Scene.h>
#include <filament/TransformManager.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/materials/uberarchive.h>
#include <utils/Entity.h>
#endif

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace tiles_renderer {
namespace {

#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)

constexpr std::uint32_t kGltfMagic = 0x46546C67; // "glTF" little-endian
constexpr double kPi = 3.14159265358979323846;

// Minimal ITaskProcessor for the SDK: each task runs on its own thread.
// Adequate for local-file tilesets; a host app with its own job system can
// replace it later (TODO).
class SimpleTaskProcessor : public CesiumAsync::ITaskProcessor {
public:
    void startTask(std::function<void()> f) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _threads.emplace_back([f = std::move(f)]() { f(); });
    }

    ~SimpleTaskProcessor() override {
        for (std::thread& t : _threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

private:
    std::mutex _mutex;
    std::vector<std::thread> _threads;
};

// ---------------------------------------------------------------------------
// LocalFileAssetAccessor: serves tileset.json / tile content from the local
// filesystem. URLs may be plain paths or file:// URLs (cesium-native resolves
// relative content URIs against the tileset URL, so we see absolute paths).
// ---------------------------------------------------------------------------
class LocalFileAssetAccessor : public CesiumAsync::IAssetAccessor {
public:
    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> get(
        const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url,
        const std::vector<THeader>& /*headers*/) override {
        std::string path = url;
        constexpr const char* kScheme = "file://";
        if (path.compare(0, 7, kScheme) == 0) {
            path = path.substr(7);
        }
        std::vector<std::byte> bytes;
        std::string contentType;
        std::uint16_t status = 200;
        std::ifstream in(path, std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            const auto size = in.tellg();
            in.seekg(0, std::ios::beg);
            bytes.resize(static_cast<std::size_t>(size));
            in.read(reinterpret_cast<char*>(bytes.data()), size);
            if (path.size() >= 5 &&
                path.compare(path.size() - 5, 5, ".json") == 0) {
                contentType = "application/json";
            } else if (
                path.size() >= 4 &&
                path.compare(path.size() - 4, 4, ".glb") == 0) {
                contentType = "model/gltf-binary";
            } else {
                contentType = "application/octet-stream";
            }
        } else {
            status = 404;
        }
        auto pResponse = std::make_shared<LocalResponse>(
            status, std::move(contentType), std::move(bytes));
        auto pRequest = std::make_shared<LocalRequest>(url, pResponse);
        return asyncSystem.createResolvedFuture<
            std::shared_ptr<CesiumAsync::IAssetRequest>>(std::move(pRequest));
    }

    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> request(
        const CesiumAsync::AsyncSystem& asyncSystem,
        const std::string& /*verb*/, const std::string& url,
        const std::vector<THeader>& /*headers*/,
        const std::span<const std::byte>& /*contentPayload*/) override {
        // Local files are read-only; POST etc. are unsupported.
        auto pResponse = std::make_shared<LocalResponse>(
            405, "text/plain", std::vector<std::byte>{});
        auto pRequest = std::make_shared<LocalRequest>(url, pResponse);
        return asyncSystem.createResolvedFuture<
            std::shared_ptr<CesiumAsync::IAssetRequest>>(std::move(pRequest));
    }

    void tick() noexcept override {}

private:
    class LocalResponse : public CesiumAsync::IAssetResponse {
    public:
        LocalResponse(
            std::uint16_t status, std::string contentType,
            std::vector<std::byte> data)
            : _status(status),
              _contentType(std::move(contentType)),
              _data(std::move(data)) {}
        std::uint16_t statusCode() const override { return _status; }
        std::string contentType() const override { return _contentType; }
        const CesiumAsync::HttpHeaders& headers() const override {
            return _headers;
        }
        std::span<const std::byte> data() const override { return _data; }

    private:
        std::uint16_t _status;
        std::string _contentType;
        CesiumAsync::HttpHeaders _headers;
        std::vector<std::byte> _data;
    };

    class LocalRequest : public CesiumAsync::IAssetRequest {
    public:
        LocalRequest(
            std::string url,
            std::shared_ptr<CesiumAsync::IAssetResponse> pResponse)
            : _url(std::move(url)), _pResponse(std::move(pResponse)) {}
        const std::string& method() const override { return _method; }
        const std::string& url() const override { return _url; }
        const CesiumAsync::HttpHeaders& headers() const override {
            return _headers;
        }
        const CesiumAsync::IAssetResponse* response() const override {
            return _pResponse.get();
        }

    private:
        std::string _method = "GET";
        std::string _url;
        CesiumAsync::HttpHeaders _headers;
        std::shared_ptr<CesiumAsync::IAssetResponse> _pResponse;
    };
};

// ---------------------------------------------------------------------------
// Per-tile render data, created in prepareInMainThread and freed in free().
// The glb bytes are kept alive for the asset's lifetime (gltfio references
// the caller's buffer).
// ---------------------------------------------------------------------------
struct TileRenderData {
    std::vector<std::uint8_t> glbBytes;
    filament::gltfio::FilamentAsset* asset = nullptr;
    bool inScene = false;
};

// ---------------------------------------------------------------------------
// FilamentPrepareResources: IPrepareRendererResources implementation that
// converts each tile's glb content into a gltfio FilamentAsset.
// ---------------------------------------------------------------------------
class FilamentPrepareResources
    : public Cesium3DTilesSelection::IPrepareRendererResources {
public:
    FilamentPrepareResources(filament::Engine* engine, filament::Scene* scene)
        : _engine(engine), _scene(scene) {
        _materialProvider = filament::gltfio::createUbershaderProvider(
            engine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
        filament::gltfio::AssetConfiguration assetConfig{};
        assetConfig.engine = engine;
        assetConfig.materials = _materialProvider;
        _assetLoader = filament::gltfio::AssetLoader::create(assetConfig);
        filament::gltfio::ResourceConfiguration resourceConfig{};
        resourceConfig.engine = engine;
        _resourceLoader =
            new filament::gltfio::ResourceLoader(resourceConfig);
    }

    ~FilamentPrepareResources() override {
        // All tile assets must have been freed via free() before this runs
        // (Tileset destruction unloads tiles first).
        _materialProvider->destroyMaterials();
        delete _materialProvider;
        filament::gltfio::AssetLoader::destroy(&_assetLoader);
        delete _resourceLoader;
    }

    CesiumAsync::Future<
        Cesium3DTilesSelection::TileLoadResultAndRenderResources>
    prepareInLoadThread(
        const CesiumAsync::AsyncSystem& asyncSystem,
        Cesium3DTilesSelection::TileLoadResult&& tileLoadResult,
        const glm::dmat4& /*transform*/,
        const std::any& /*rendererOptions*/) override {
        void* pBytes = nullptr;
        const auto& result = tileLoadResult;
        if (result.pCompletedRequest != nullptr &&
            result.pCompletedRequest->response() != nullptr) {
            const auto data = result.pCompletedRequest->response()->data();
            if (data.size() >= 4) {
                std::uint32_t magic = 0;
                std::memcpy(&magic, data.data(), 4);
                if (magic == kGltfMagic) {
                    auto* pVec = new std::vector<std::uint8_t>(
                        data.size());
                    std::memcpy(
                        pVec->data(), data.data(), data.size());
                    pBytes = pVec;
                }
            }
        }
        Cesium3DTilesSelection::TileLoadResultAndRenderResources out;
        out.result = std::move(tileLoadResult);
        out.pRenderResources = pBytes;
        return asyncSystem.createResolvedFuture<
            Cesium3DTilesSelection::TileLoadResultAndRenderResources>(
            std::move(out));
    }

    void* prepareInMainThread(
        Cesium3DTilesSelection::Tile& tile, void* pLoadThreadResult) override {
        auto* pBytes =
            static_cast<std::vector<std::uint8_t>*>(pLoadThreadResult);
        if (pBytes == nullptr) {
            return nullptr; // not glb content (or load failed)
        }
        auto* pData = new TileRenderData();
        pData->glbBytes = std::move(*pBytes);
        delete pBytes;

        pData->asset = _assetLoader->createAsset(
            pData->glbBytes.data(),
            static_cast<std::uint32_t>(pData->glbBytes.size()));
        if (pData->asset == nullptr) {
            std::cerr << "[tiles_renderer] prepareInMainThread: gltfio "
                         "createAsset failed"
                      << std::endl;
            delete pData;
            return nullptr;
        }
        if (!_resourceLoader->loadResources(pData->asset)) {
            std::cerr << "[tiles_renderer] prepareInMainThread: gltfio "
                         "loadResources failed"
                      << std::endl;
            _assetLoader->destroyAsset(pData->asset);
            delete pData;
            return nullptr;
        }

        // Apply the tile's transform to the glTF root (identity for the P3
        // test tileset; non-identity for real tilesets).
        const glm::dmat4& t = tile.getTransform();
        filament::math::mat4f m;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                m[c][r] = static_cast<float>(t[c][r]);
            }
        }
        auto& transformManager = _engine->getTransformManager();
        const auto rootInstance =
            transformManager.getInstance(pData->asset->getRoot());
        if (rootInstance.isValid()) {
            transformManager.setTransform(rootInstance, m);
        }
        return pData;
    }

    void free(
        Cesium3DTilesSelection::Tile& /*tile*/, void* pLoadThreadResult,
        void* pMainThreadResult) noexcept override {
        // Case 1: prepareInMainThread never ran — drop the raw bytes.
        delete static_cast<std::vector<std::uint8_t>*>(pLoadThreadResult);
        // Case 2: full render data — remove from scene, destroy asset.
        auto* pData = static_cast<TileRenderData*>(pMainThreadResult);
        if (pData == nullptr) {
            return;
        }
        if (pData->inScene && pData->asset != nullptr) {
            const utils::Entity* entities = pData->asset->getEntities();
            _scene->removeEntities(entities, pData->asset->getEntityCount());
            pData->inScene = false;
        }
        if (pData->asset != nullptr) {
            _assetLoader->destroyAsset(pData->asset);
        }
        delete pData;
    }

    // No raster overlays in P3.
    void* prepareRasterInLoadThread(
        CesiumImage::ImageAsset& /*image*/,
        const std::any& /*rendererOptions*/) override {
        return nullptr;
    }
    void* prepareRasterInMainThread(
        CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
        void* pLoadThreadResult) override {
        return pLoadThreadResult;
    }
    void freeRaster(
        const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
        void* /*pLoadThreadResult*/,
        void* /*pMainThreadResult*/) noexcept override {}
    void attachRasterInMainThread(
        const Cesium3DTilesSelection::Tile& /*tile*/,
        int32_t /*overlayTextureCoordinateID*/,
        const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
        void* /*pMainThreadRendererResources*/,
        const glm::dvec2& /*translation*/,
        const glm::dvec2& /*scale*/) override {}
    void detachRasterInMainThread(
        const Cesium3DTilesSelection::Tile& /*tile*/,
        int32_t /*overlayTextureCoordinateID*/,
        const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
        void* /*pMainThreadRendererResources*/) noexcept override {}

private:
    filament::Engine* _engine;
    filament::Scene* _scene;
    filament::gltfio::MaterialProvider* _materialProvider = nullptr;
    filament::gltfio::AssetLoader* _assetLoader = nullptr;
    filament::gltfio::ResourceLoader* _resourceLoader = nullptr;
};

#endif // TILES_WITH_CESIUM_NATIVE && TILES_WITH_FILAMENT

} // namespace

// ---------------------------------------------------------------------------
// TilesetRenderer::Impl
// ---------------------------------------------------------------------------
struct TilesetRenderer::Impl {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    Impl(filament::Engine* engine_, filament::Scene* scene_)
        : engine(engine_), scene(scene_) {}

    bool loadTileset(const std::string& urlOrPath) {
        std::string url = urlOrPath;
        std::string localPath;
        if (url.compare(0, 7, "file://") == 0) {
            localPath = url.substr(7);
        } else if (url.find("://") == std::string::npos) {
            localPath = url;
            url = "file://" + url;
        }
        if (!localPath.empty()) {
            std::ifstream probe(localPath, std::ios::binary);
            if (!probe) {
                std::cerr << "[tiles_renderer] loadTileset: file not found: "
                          << urlOrPath << std::endl;
                return false;
            }
        }
        // Register cesium-native's tile content converters (glTF, b3dm, etc.).
        // Without this, GLB magic bytes are not recognized and tile loads
        // fail. Must be called once before any Tileset is created.
        static bool contentTypesRegistered = false;
        if (!contentTypesRegistered) {
            Cesium3DTilesContent::registerAllTileContentTypes();
            contentTypesRegistered = true;
        }

        auto pAccessor = std::make_shared<LocalFileAssetAccessor>();
        auto pPrepare =
            std::make_shared<FilamentPrepareResources>(engine, scene);
        // Keep an AsyncSystem handle: TilesetExternals takes a copy (it is a
        // shared-ownership wrapper), we keep ours for pumping main-thread
        // tasks every frame.
        taskProcessor = std::make_shared<SimpleTaskProcessor>();
        asyncSystem.emplace(taskProcessor);
        Cesium3DTilesSelection::TilesetExternals externals{
            pAccessor, pPrepare, *asyncSystem,
            std::make_shared<CesiumUtility::CreditSystem>()};
        // Keep the prepare resources alive as long as the tileset: the
        // Tileset only holds the shared_ptr from externals during
        // construction, so retain our own copy too.
        prepareResources = pPrepare;

        Cesium3DTilesSelection::TilesetOptions options;
        tileset = std::make_unique<Cesium3DTilesSelection::Tileset>(
            externals, url, options);
        // Wait (bounded) for the root tile metadata so load() can report
        // success/failure honestly instead of always succeeding.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (tileset->getRootTile() == nullptr &&
               std::chrono::steady_clock::now() < deadline) {
            asyncSystem->dispatchMainThreadTasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (tileset->getRootTile() == nullptr) {
            std::cerr << "[tiles_renderer] loadTileset: failed to load "
                      << urlOrPath << std::endl;
            tileset.reset();
            return false;
        }
        std::cout << "[tiles_renderer] tileset loaded: " << urlOrPath
                  << std::endl;
        loaded = true;
        return true;
    }

    void updateTiles(
        double viewportWidth, double viewportHeight, const OrbitCamera& cam) {
        if (tileset == nullptr) {
            return;
        }
        // Pump async work (tileset.json fetch, tile content loads).
        asyncSystem->dispatchMainThreadTasks();

        const Cesium3DTilesSelection::Tile* pRoot = tileset->getRootTile();
        if (pRoot == nullptr) {
            return;
        }

        // Orbit camera -> cesium ViewState.
        const double yaw = cam.yawDegrees * kPi / 180.0;
        const double pitch = cam.pitchDegrees * kPi / 180.0;
        const glm::dvec3 target(cam.targetX, cam.targetY, cam.targetZ);
        const glm::dvec3 eye(
            target.x + cam.distance * std::cos(pitch) * std::sin(yaw),
            target.y + cam.distance * std::sin(pitch),
            target.z + cam.distance * std::cos(pitch) * std::cos(yaw));
        const glm::dvec3 direction = glm::normalize(target - eye);
        const glm::dvec3 up(0.0, 1.0, 0.0);
        constexpr double kVfov = 45.0 * kPi / 180.0; // matches SDK camera
        const double aspect = viewportWidth / viewportHeight;
        const double kHfov = 2.0 * std::atan(std::tan(kVfov / 2.0) * aspect);
        Cesium3DTilesSelection::ViewState viewState(
            eye, direction, up, glm::dvec2(viewportWidth, viewportHeight),
            kHfov, kVfov);
        const Cesium3DTilesSelection::ViewUpdateResult& viewResult =
            tileset->updateViewGroup(
                tileset->getDefaultViewGroup(), {viewState});
        // updateViewGroup only fills the traversal's load queue; loadTiles()
        // actually starts/processes the queued tile content loads.
        tileset->loadTiles();

        // Use the traversal's explicit render selection to drive Scene
        // visibility (not tile.isRenderable(), which is true for every loaded
        // tile). Include fading-out tiles so LOD transitions don't pop.
        std::unordered_set<Cesium3DTilesSelection::Tile::ConstPointer>
            wantVisible;
        for (const auto& pTile : viewResult.tilesToRenderThisFrame) {
            wantVisible.insert(pTile);
        }
        for (const auto& pTile : viewResult.tilesFadingOut) {
            wantVisible.insert(pTile);
        }

        // Toggle per-tile visibility from the traversal's render selection.
        renderedCount = 0;
        updateTileVisibility(
            const_cast<Cesium3DTilesSelection::Tile&>(*pRoot), wantVisible);
    }

    void updateTileVisibility(
        Cesium3DTilesSelection::Tile& tile,
        const std::unordered_set<
            Cesium3DTilesSelection::Tile::ConstPointer>& wantVisible) {
        auto* pContent = tile.getContent().getRenderContent();
        if (pContent != nullptr) {
            auto* pData = static_cast<TileRenderData*>(
                pContent->getRenderResources());
            if (pData != nullptr && pData->asset != nullptr) {
                // Tile::ConstPointer is shared_ptr<const Tile>; get the raw
                // pointer for set lookup via a temporary const view.
                const Cesium3DTilesSelection::Tile* pRaw = &tile;
                bool want = false;
                for (const auto& p : wantVisible) {
                    if (p.get() == pRaw) {
                        want = true;
                        break;
                    }
                }
                if (want && !pData->inScene) {
                    scene->addEntities(
                        pData->asset->getEntities(),
                        pData->asset->getEntityCount());
                    pData->inScene = true;
                } else if (!want && pData->inScene) {
                    scene->removeEntities(
                        pData->asset->getEntities(),
                        pData->asset->getEntityCount());
                    pData->inScene = false;
                }
                if (pData->inScene) {
                    ++renderedCount;
                }
            }
        }
        for (Cesium3DTilesSelection::Tile& child : tile.getChildren()) {
            updateTileVisibility(child, wantVisible);
        }
    }

    filament::Engine* engine = nullptr;
    filament::Scene* scene = nullptr;
    // Destruction order matters (reverse of declaration): tileset first,
    // then asyncSystem, then taskProcessor — so background tasks are joined
    // only after the tileset is gone.
    std::shared_ptr<SimpleTaskProcessor> taskProcessor;
    std::optional<CesiumAsync::AsyncSystem> asyncSystem;
    std::shared_ptr<FilamentPrepareResources> prepareResources;
    std::unique_ptr<Cesium3DTilesSelection::Tileset> tileset;
    bool loaded = false;
    int renderedCount = -1;
#else
    Impl(filament::Engine*, filament::Scene*) {}
#endif
};

TilesetRenderer::TilesetRenderer(filament::Engine* engine, filament::Scene* scene)
    : _impl(std::make_unique<Impl>(engine, scene)) {}

TilesetRenderer::~TilesetRenderer() = default;

bool TilesetRenderer::load(const std::string& urlOrPath) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->loadTileset(urlOrPath);
#else
    (void)urlOrPath;
    std::cerr << "[tiles_renderer] loadTileset: not available (built without "
                 "cesium-native + Filament)"
              << std::endl;
    return false;
#endif
}

void TilesetRenderer::update(
    double viewportWidth, double viewportHeight, const OrbitCamera& camera) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    _impl->updateTiles(viewportWidth, viewportHeight, camera);
#else
    (void)viewportWidth;
    (void)viewportHeight;
    (void)camera;
#endif
}

bool TilesetRenderer::isLoaded() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->loaded;
#else
    return false;
#endif
}

int TilesetRenderer::renderedTileCount() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->renderedCount;
#else
    return -1;
#endif
}

} // namespace tiles_renderer
