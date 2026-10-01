// tiles_renderer SDK — 3D Tiles tileset integration (P3).
//
// Loads a tileset.json via cesium-native, selects tiles per frame with
// updateViewGroup, and converts each tile's glb content into Filament
// renderables with gltfio (+ ubershader materials).

#include "tileset_internal.h"
#include "converter_guard.h" // P28: empty-model guard for content converters
#include "tilesetio/cesium_adapter.h"
#include "tilesetio/filament_backend.h"

#include <mutex> // P32: loadErrorCallback queue (may arrive off-thread)
#include <unordered_map> // P32: per-tile last-state tracking for events
#include <unordered_set>

#ifdef TILES_WITH_CESIUM_NATIVE
#include <Cesium3DTilesSelection/BoundingVolume.h>
#include <Cesium3DTilesSelection/IPrepareRendererResources.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/TileContent.h>
#include <Cesium3DTilesSelection/TileID.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/TilesetOptions.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <CesiumGeospatial/Ellipsoid.h> // P31: TilesetOptions.ellipsoid
#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/IAssetResponse.h>
#include <CesiumAsync/ThreadPool.h>
#include <CesiumGltf/ExtensionCesiumRTC.h>
#include <CesiumGltf/ExtensionExtMeshGpuInstancing.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltfWriter/GltfWriter.h>
#include <CesiumUtility/CreditSystem.h>
#include <rapidjson/document.h> // P23: pre-flight tileset.json validation
#include <rapidjson/writer.h>     // P37-B: GLB JSON re-serialization
#include <rapidjson/stringbuffer.h>
#include <functional> // P37-B: recursive extension stripping
#include <cstring>    // P37-B: memcpy/strcmp in GLB patching
// P22-hotfix: glm and curl are only available when cesium-native is built
// (glm arrives via cesium-native's vcpkg tree; curl via its vcpkg ports).
// The Windows/Android/iOS CI configs build the SDK WITHOUT
// cesium-native, so these includes must stay inside this guard — an
// unconditional include here broke all three platforms (C1083 /
// 'glm/gtc/matrix_transform.hpp' file not found, P22 CI).
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <curl/curl.h>
#endif

#ifdef TILES_WITH_FILAMENT
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/ResourceLoader.h>
#if defined(TILES_WITH_STB_PROVIDER) || defined(TILES_WITH_KTX2_PROVIDER)
#include <gltfio/TextureProvider.h> // createStbProvider (P15) / createKtx2Provider (P25)
#endif
#ifdef TILES_WITH_KTX2_LIBKTX
#include "ktx2_provider_internal.h" // P25: libktx-backed KTX2 provider
#endif
#include <gltfio/materials/uberarchive.h>
#include <utils/Entity.h>
#include <utils/EntityManager.h>
#include "unlit_color_filamat.h" // P36: debug line-box material (matc)
#endif

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

#include <filesystem>
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
// NonThrowingCurlAccessor: minimal HTTP(S) client for tile fetching.
//
// cesium-native's CurlAssetAccessor throws std::runtime_error when the
// network itself fails (DNS, connection refused, timeout...). On Linux our
// process mixes libstdc++ (our code, GCC) with libc++ (Filament prebuilts);
// the thrown exception's destructor is interposed to libc++abi's version,
// which frees libstdc++-allocated memory with free() -> heap corruption /
// SIGSEGV (found by P18's outage test; see docs/adr/0016-weak-network-testing.md).
// This accessor NEVER throws: network failures are reported as synthetic
// HTTP 599 responses with empty bodies, which the cesium-native loaders
// already handle gracefully (the same path as a real HTTP 500).
// ---------------------------------------------------------------------------
class SimpleAssetResponse : public CesiumAsync::IAssetResponse {
public:
    SimpleAssetResponse(std::uint16_t status, std::string contentType,
                        std::vector<std::byte> data)
        : _status(status), _contentType(std::move(contentType)),
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

class SimpleAssetRequest : public CesiumAsync::IAssetRequest {
public:
    SimpleAssetRequest(std::string method, std::string url,
                       const std::vector<CesiumAsync::IAssetAccessor::THeader>& headers,
                       std::unique_ptr<SimpleAssetResponse> response)
        : _method(std::move(method)), _url(std::move(url)),
          _response(std::move(response)) {
        for (const auto& h : headers) {
            _headers[h.first] = h.second;
        }
    }

    const std::string& method() const override { return _method; }
    const std::string& url() const override { return _url; }
    const CesiumAsync::HttpHeaders& headers() const override {
        return _headers;
    }
    const CesiumAsync::IAssetResponse* response() const override {
        return _response.get();
    }

private:
    std::string _method;
    std::string _url;
    CesiumAsync::HttpHeaders _headers;
    std::unique_ptr<SimpleAssetResponse> _response;
};

class NonThrowingCurlAccessor : public CesiumAsync::IAssetAccessor {
public:
    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> get(
        const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url,
        const std::vector<THeader>& headers) override {
        return asyncSystem.runInWorkerThread(
            [url, headers]() -> std::shared_ptr<CesiumAsync::IAssetRequest> {
                return perform(
                    "GET", url, headers, std::span<const std::byte>());
            });
    }

    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> request(
        const CesiumAsync::AsyncSystem& asyncSystem, const std::string& verb,
        const std::string& url, const std::vector<THeader>& headers,
        const std::span<const std::byte>& contentPayload) override {
        // Copy the payload: the caller's span may not outlive the worker.
        std::vector<std::byte> payload(
            contentPayload.begin(), contentPayload.end());
        return asyncSystem.runInWorkerThread(
            [verb, url, headers,
             payload = std::move(payload)]()
                -> std::shared_ptr<CesiumAsync::IAssetRequest> {
                return perform(verb, url, headers, payload);
            });
    }

    void tick() noexcept override {
        // All transfers block inside worker threads; nothing to pump.
    }

private:
    // Synthetic status for "the network itself failed" (refused, DNS,
    // timeout...). Outside the real 1xx-5xx range so it can't be confused
    // with a server reply; loaders treat any non-2xx as a failed load.
    static constexpr std::uint16_t kNetworkErrorStatus = 599;

    static std::size_t writeCallback(
        char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
        auto* data = static_cast<std::vector<std::byte>*>(userdata);
        std::size_t n = size * nmemb;
        const std::byte* bytes = reinterpret_cast<const std::byte*>(ptr);
        data->insert(data->end(), bytes, bytes + n);
        return n;
    }

    static void ensureCurlInit() {
        static std::once_flag flag;
        std::call_once(flag, []() {
            curl_global_init(CURL_GLOBAL_DEFAULT);
        });
    }

    static std::shared_ptr<CesiumAsync::IAssetRequest> perform(
        const std::string& verb, const std::string& url,
        const std::vector<THeader>& headers,
        std::span<const std::byte> payload) noexcept {
        // Must not throw: see the class comment. All failure paths below
        // synthesize a 599 response instead.
        ensureCurlInit();
        CURL* curl = curl_easy_init();
        if (!curl) {
            return makeError(verb, url, headers);
        }

        std::vector<std::byte> data;
        std::string contentType;
        long httpStatus = 0;
        bool ok = false;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        // Fail fast on stalled connections so a dead server surfaces as a
        // failed tile instead of hanging a worker (outage recovery).
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);

        struct curl_slist* list = nullptr;
        for (const auto& h : headers) {
            std::string line = h.first + ": " + h.second;
            list = curl_slist_append(list, line.c_str());
        }
        if (list) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        }

        if (verb != "GET") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verb.c_str());
            if (!payload.empty()) {
                curl_easy_setopt(
                    curl, CURLOPT_POSTFIELDS, payload.data());
                curl_easy_setopt(
                    curl, CURLOPT_POSTFIELDSIZE,
                    static_cast<long>(payload.size()));
            }
        }

        if (curl_easy_perform(curl) == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
            char* ct = nullptr;
            curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
            if (ct) {
                contentType = ct;
            }
            ok = true;
        }
        if (list) {
            curl_slist_free_all(list);
        }
        curl_easy_cleanup(curl);

        std::uint16_t status =
            ok ? static_cast<std::uint16_t>(httpStatus)
               : kNetworkErrorStatus;
        if (!ok) {
            data.clear();
            contentType.clear();
        }
        auto response = std::make_unique<SimpleAssetResponse>(
            status, std::move(contentType), std::move(data));
        return std::make_shared<SimpleAssetRequest>(
            verb, url, headers, std::move(response));
    }

    static std::shared_ptr<CesiumAsync::IAssetRequest> makeError(
        const std::string& verb, const std::string& url,
        const std::vector<THeader>& headers) noexcept {
        auto response = std::make_unique<SimpleAssetResponse>(
            kNetworkErrorStatus, std::string(), std::vector<std::byte>());
        return std::make_shared<SimpleAssetRequest>(
            verb, url, headers, std::move(response));
    }
};

// ---------------------------------------------------------------------------
// RoutingAssetAccessor: http(s) URLs go through the SDK's NonThrowingCurlAccessor
// (libcurl, blocking worker threads); everything else (plain paths, file://)
// is served from the local filesystem.
// ---------------------------------------------------------------------------
class RoutingAssetAccessor : public CesiumAsync::IAssetAccessor {
public:
    RoutingAssetAccessor()
        : _pCurl(std::make_shared<NonThrowingCurlAccessor>()),
          _pLocal(std::make_shared<LocalFileAssetAccessor>()) {}

    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> get(
        const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url,
        const std::vector<THeader>& headers) override {
        if (isHttp(url)) {
            return _pCurl->get(asyncSystem, url, headers);
        }
        return _pLocal->get(asyncSystem, url, headers);
    }

    CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> request(
        const CesiumAsync::AsyncSystem& asyncSystem, const std::string& verb,
        const std::string& url, const std::vector<THeader>& headers,
        const std::span<const std::byte>& contentPayload) override {
        if (isHttp(url)) {
            return _pCurl->request(
                asyncSystem, verb, url, headers, contentPayload);
        }
        return _pLocal->request(
            asyncSystem, verb, url, headers, contentPayload);
    }

    void tick() noexcept override {
        _pCurl->tick();
        _pLocal->tick();
    }

private:
    static bool isHttp(const std::string& url) {
        return url.compare(0, 7, "http://") == 0 ||
               url.compare(0, 8, "https://") == 0;
    }

    std::shared_ptr<NonThrowingCurlAccessor> _pCurl;
    std::shared_ptr<LocalFileAssetAccessor> _pLocal;
};

// ---------------------------------------------------------------------------
// P23: fail-fast pre-flight for the root tileset.json.
//
// P22's build-then-commit waits (bounded, 30s) for the replacement
// tileset's root tile before committing. For a corrupt-but-present
// tileset.json that wait always ran to the full 30s: the bytes arrive
// fine, cesium-native's TilesetJsonLoader fails the parse in a worker
// thread, and nothing distinguishes "still loading" from "load failed" —
// getRootTile() stays nullptr either way.
//
// Fix: validate the root document BEFORE constructing the Tileset.
//   * local file: read + rapidjson parse (synchronous, ~ms).
//   * http(s): one blocking fetch through the same RoutingAssetAccessor
//     the Tileset will use (same curl timeouts, never throws — a network
//     failure surfaces as synthetic 599), then the same parse.
// The check mirrors what the loader needs: a parseable JSON object with
// a "root" object member. Anything weaker (garbage bytes, truncated
// JSON, valid JSON that isn't a tileset) fails here in milliseconds
// instead of spinning the 30s root-wait. Slow networks are unaffected:
// the pre-flight fetch uses the same generous timeouts as tile loading,
// so a slow-but-valid root still passes (P18's slow_http_server scenario
// keeps working).
// ---------------------------------------------------------------------------
bool preflightTilesetRoot(
    const std::string& url,
    const std::string& localPath,
    RoutingAssetAccessor& accessor,
    CesiumAsync::AsyncSystem& asyncSystem,
    std::string& errorOut) {
    std::vector<std::byte> bytes;
    if (!localPath.empty()) {
        std::ifstream in(localPath, std::ios::binary);
        if (!in) {
            errorOut = "file not found: " + localPath;
            return false;
        }
        in.seekg(0, std::ios::end);
        const auto size = in.tellg();
        if (size < 0) {
            errorOut = "cannot read file: " + localPath;
            return false;
        }
        in.seekg(0, std::ios::beg);
        bytes.resize(static_cast<std::size_t>(size));
        if (!bytes.empty()) {
            in.read(reinterpret_cast<char*>(bytes.data()), size);
        }
    } else {
        // HTTP(S): blocking fetch through the same accessor. The curl
        // path never throws (network failure -> synthetic 599), but guard
        // anyway — loadTileset must not let exceptions escape.
        std::shared_ptr<CesiumAsync::IAssetRequest> pRequest;
        try {
            pRequest = accessor.get(asyncSystem, url, {}).wait();
        } catch (const std::exception& e) {
            errorOut = std::string("failed to fetch tileset.json: ") + e.what();
            return false;
        } catch (...) {
            errorOut = "failed to fetch tileset.json: unknown error";
            return false;
        }
        const CesiumAsync::IAssetResponse* pResponse =
            pRequest ? pRequest->response() : nullptr;
        if (pResponse == nullptr) {
            errorOut = "failed to fetch tileset.json: empty response";
            return false;
        }
        const std::uint16_t code = pResponse->statusCode();
        if (code < 200 || code >= 300) {
            errorOut = "failed to fetch tileset.json: HTTP " +
                       std::to_string(code);
            return false;
        }
        const auto data = pResponse->data();
        bytes.assign(data.begin(), data.end());
    }
    if (bytes.empty()) {
        errorOut = "tileset.json is empty";
        return false;
    }
    rapidjson::Document doc;
    doc.Parse(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (doc.HasParseError()) {
        errorOut = "tileset.json is not valid JSON (parse error at byte " +
                   std::to_string(doc.GetErrorOffset()) + ")";
        return false;
    }
    if (!doc.IsObject() || !doc.HasMember("root") ||
        !doc["root"].IsObject()) {
        errorOut = "tileset.json is not a 3D Tiles tileset (missing \"root\")";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-tile render data, created in prepareInMainThread and freed in free().
// T1 (tilesetio): holds FilamentBackend resources (entities, GPU buffers).
// rtcCenter/upAxisFix are the double-precision transform pieces from
// LoadThreadData, kept so setModelMatrix() (P33) can recompose the tile's
// render transform without re-loading its content.
// ---------------------------------------------------------------------------
struct TileRenderData {
    tilesetio::FilamentTileResources filamentResources;
    bool inScene = false;
    // T1 (tilesetio): tile-level transform (double[16], column-major).
    // Applied to the asset root when adding to scene.
    double tileTransform[16];
    // P35: whether this tile's debug wireframe (getWireframe()) is in the
    // scene. Managed by updateTileVisibility / updateWireframeVisibility.
    // P36: re-gated on debugShowContentBoundingVolume (was
    // debugShowBoundingVolume in P35; see ADR-0035).
    bool wireframeInScene = false;
    // P36: debug line-box entities for the tile bounding volume (tileset.json)
    // and the viewer request volume. Created lazily by
    // updateTileBoundingVolume / updateTileRequestVolume, destroyed in free().
    // The entity transform carries the volume, so no per-tile geometry.
    utils::Entity bvEntity;
    bool bvInScene = false;
    utils::Entity rqEntity;
    bool rqInScene = false;
    glm::dvec3 rtcCenter{0.0, 0.0, 0.0};
    glm::dmat4 upAxisFix{1.0};
};

// ---------------------------------------------------------------------------
// Load-thread result handed from prepareInLoadThread to prepareInMainThread.
// T1 (tilesetio): holds the CesiumGltf::Model directly — no GLB round-trip.
// The Model is moved from the TileLoadResult. rtcCenter/upAxisFix are the
// same as before (tile-level transform pieces).
// ---------------------------------------------------------------------------
struct LoadThreadData {
    // For Model content: the converted model (moved from TileLoadResult).
    // For raw GLB: parsed into a Model via cesium-native's GltfReader.
    std::optional<CesiumGltf::Model> model;
    glm::dvec3 rtcCenter{0.0, 0.0, 0.0};
    glm::dmat4 upAxisFix{1.0};
};

// ---------------------------------------------------------------------------
// P7: the rotation the i3dm converter conjugates instance transforms with.
//
// I3dmToGltfConverter writes each instance transform as
//   toTileInv * composeInstanceTransform(i) * toTile
// with toTile = upToZ * nodeTransform, where upToZ =
// GltfUtilities::applyGltfUpAxisTransform(model, identity). It assumes the
// runtime applies the same upToZ at the tile root (cesium-native's own
// computeBoundingRegion uses tileTransform * T(rtc) * upToZ as the content
// root), which cancels the conjugation. Our world is Y-up like the
// embedded glTF, and the b3dm converter performs no such conjugation, so
// for i3dm-converted models the render bridge must apply upToZ at the
// asset root itself. Without it, instances render rotated by
// inverse(upToZ): for RTC_CENTER-scale offsets that throws them megameters
// off (empty frame), and even for small tilesets it pushes content
// outside its own bounding volume. Replicated here (column-major, must
// match CesiumGeometry::Transforms) so the SDK doesn't pull in
// CesiumGltfContent for a single matrix.
// ---------------------------------------------------------------------------
glm::dmat4 upAxisToZUp(const CesiumGltf::Model& model) {
    int axis = 1; // Y — the glTF default and cesium-native's default
    const auto it = model.extras.find("gltfUpAxis");
    if (it != model.extras.end()) {
        axis = static_cast<int>(it->second.getSafeNumberOrDefault(1));
    }
    if (axis == 0) { // X up -> Z up
        return glm::dmat4(
            glm::dvec4(0.0, 0.0, 1.0, 0.0),
            glm::dvec4(0.0, 1.0, 0.0, 0.0),
            glm::dvec4(-1.0, 0.0, 0.0, 0.0),
            glm::dvec4(0.0, 0.0, 0.0, 1.0));
    }
    if (axis == 2) { // Z up -> Z up: identity
        return glm::dmat4(1.0);
    }
    // Y up -> Z up
    return glm::dmat4(
        glm::dvec4(1.0, 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 1.0, 0.0),
        glm::dvec4(0.0, -1.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 0.0, 1.0));
}

// ---------------------------------------------------------------------------
// P7: expand EXT_mesh_gpu_instancing into plain nodes.
//
// Filament v1.77's gltfio does NOT implement EXT_mesh_gpu_instancing
// (verified in the prebuilt libgltfio_core.a: the extension name appears
// once in the extension registry, but the instance TRANSLATION/ROTATION/
// SCALE attributes are never read). It renders the base mesh exactly once
// and silently drops every instance. The render bridge therefore expands
// each instanced node into N regular nodes with baked
// (nodeMatrix * instanceTRS) matrices, which gltfio renders correctly.
// Cost: N draw calls instead of one instanced draw call; tile instance
// counts are small enough that this is the right correctness-first trade.
// A future Filament whose gltfio implements the extension can delete this
// function and pass the extension through untouched.
// ---------------------------------------------------------------------------
void expandGpuInstancing(CesiumGltf::Model& model) {
    using namespace CesiumGltf;
    constexpr const char* kExt =
        ExtensionExtMeshGpuInstancing::ExtensionName;

    std::vector<std::uint32_t> instancedNodes;
    for (std::uint32_t i = 0; i < model.nodes.size(); ++i) {
        if (model.nodes[i].getExtension<ExtensionExtMeshGpuInstancing>() !=
            nullptr) {
            instancedNodes.push_back(i);
        }
    }
    if (instancedNodes.empty()) {
        return;
    }

    // node index -> parents (a node may be referenced more than once, so
    // splice by search-and-replace at patch time rather than by slot).
    struct ParentRef {
        bool isScene;
        std::uint32_t parent;
    };
    std::unordered_map<std::uint32_t, std::vector<ParentRef>> parents;
    for (std::uint32_t i = 0; i < model.nodes.size(); ++i) {
        for (const std::int32_t child : model.nodes[i].children) {
            if (child >= 0) {
                parents[static_cast<std::uint32_t>(child)].push_back(
                    {false, i});
            }
        }
    }
    for (std::uint32_t i = 0; i < model.scenes.size(); ++i) {
        for (const std::int32_t child : model.scenes[i].nodes) {
            if (child >= 0) {
                parents[static_cast<std::uint32_t>(child)].push_back(
                    {true, i});
            }
        }
    }

    auto nodeMatrix = [](const Node& node) {
        glm::dmat4 m(1.0);
        if (node.matrix.size() == 16) {
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    m[c][r] = node.matrix[c * 4 + r];
                }
            }
            return m;
        }
        glm::dvec3 t(0.0);
        glm::dquat q(1.0, 0.0, 0.0, 0.0); // (w, x, y, z)
        glm::dvec3 s(1.0);
        if (node.translation.size() == 3) {
            t = glm::dvec3(
                node.translation[0], node.translation[1],
                node.translation[2]);
        }
        if (node.rotation.size() == 4) {
            // glTF quaternion: (x, y, z, w).
            q = glm::dquat(
                node.rotation[3], node.rotation[0], node.rotation[1],
                node.rotation[2]);
        }
        if (node.scale.size() == 3) {
            s = glm::dvec3(node.scale[0], node.scale[1], node.scale[2]);
        }
        return glm::translate(glm::dmat4(1.0), t) * glm::mat4_cast(q) *
               glm::scale(glm::dmat4(1.0), s);
    };

    for (const std::uint32_t nodeIdx : instancedNodes) {
        // Copy (not reference): appending clones below may reallocate
        // model.nodes and invalidate references.
        const Node node = model.nodes[nodeIdx];
        const auto* pExt =
            node.getExtension<ExtensionExtMeshGpuInstancing>();
        if (pExt == nullptr) {
            continue;
        }
        const auto itT = pExt->attributes.find("TRANSLATION");
        if (itT == pExt->attributes.end()) {
            continue; // no translations: nothing to expand
        }
        AccessorView<glm::vec3> translations(model, itT->second);
        if (translations.status() != AccessorViewStatus::Valid ||
            translations.size() == 0) {
            continue;
        }
        const auto itR = pExt->attributes.find("ROTATION");
        const auto itS = pExt->attributes.find("SCALE");
        AccessorView<glm::vec4> rotations(
            model, itR != pExt->attributes.end() ? itR->second : -1);
        AccessorView<glm::vec3> scales(
            model, itS != pExt->attributes.end() ? itS->second : -1);
        const bool hasR =
            rotations.status() == AccessorViewStatus::Valid;
        const bool hasS = scales.status() == AccessorViewStatus::Valid;

        const glm::dmat4 base = nodeMatrix(node);
        const std::uint64_t count =
            static_cast<std::uint64_t>(translations.size());
        std::vector<std::int32_t> clones;
        clones.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            const glm::vec3 t = translations[i];
            glm::vec4 r(0.0f, 0.0f, 0.0f, 1.0f);
            if (hasR && i < static_cast<std::uint64_t>(rotations.size())) {
                r = rotations[i];
            }
            glm::vec3 s(1.0f);
            if (hasS && i < static_cast<std::uint64_t>(scales.size())) {
                s = scales[i];
            }
            const glm::dmat4 inst =
                glm::translate(glm::dmat4(1.0), glm::dvec3(t)) *
                glm::mat4_cast(glm::dquat(r.w, r.x, r.y, r.z)) *
                glm::scale(glm::dmat4(1.0), glm::dvec3(s));
            const glm::dmat4 m = base * inst;
            Node clone = node; // copies mesh, name, children, ...
            clone.extensions.erase(kExt);
            clone.matrix.assign(
                {static_cast<double>(m[0][0]), static_cast<double>(m[0][1]),
                 static_cast<double>(m[0][2]), static_cast<double>(m[0][3]),
                 static_cast<double>(m[1][0]), static_cast<double>(m[1][1]),
                 static_cast<double>(m[1][2]), static_cast<double>(m[1][3]),
                 static_cast<double>(m[2][0]), static_cast<double>(m[2][1]),
                 static_cast<double>(m[2][2]), static_cast<double>(m[2][3]),
                 static_cast<double>(m[3][0]), static_cast<double>(m[3][1]),
                 static_cast<double>(m[3][2]), static_cast<double>(m[3][3])});
            // NOTE: write identity TRS *defaults* rather than clear()ing the
            // vectors: CesiumGltfWriter serializes any non-default vector,
            // and an empty vector != the {0,0,0}/{0,0,0,1}/{1,1,1} defaults,
            // producing invalid "translation": [] JSON that cgltf rejects.
            // Identity defaults are omitted by the writer, which is what we
            // want next to the baked matrix.
            clone.translation = {0.0, 0.0, 0.0};
            clone.rotation = {0.0, 0.0, 0.0, 1.0};
            clone.scale = {1.0, 1.0, 1.0};
            clones.push_back(static_cast<std::int32_t>(model.nodes.size()));
            model.nodes.push_back(std::move(clone));
        }

        // Splice the clones in where the instanced node was referenced.
        const auto itP = parents.find(nodeIdx);
        if (itP != parents.end()) {
            for (const ParentRef& ref : itP->second) {
                std::vector<std::int32_t>& list =
                    ref.isScene ? model.scenes[ref.parent].nodes
                                : model.nodes[ref.parent].children;
                for (auto it = list.begin(); it != list.end();) {
                    if (*it == static_cast<std::int32_t>(nodeIdx)) {
                        it = list.erase(it);
                        it = list.insert(it, clones.begin(), clones.end());
                        std::advance(it, clones.size());
                    } else {
                        ++it;
                    }
                }
            }
        }
        // The original instanced node is now unreferenced; strip the
        // extension so nothing downstream trips on it.
        model.nodes[nodeIdx].extensions.erase(kExt);
    }
    model.removeExtensionUsed(kExt);
    model.removeExtensionRequired(kExt);
}

// Serialize a cesium-native glTF Model (e.g. produced by the b3dm/i3dm
// converters) back to binary glb so gltfio can load it. A present
// CESIUM_RTC extension is extracted into rtcCenter (double) and stripped,
// because gltfio does not understand it and float glTF nodes cannot hold
// ECEF-scale centers. Returns nullptr on failure.
// ---

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
#ifdef TILES_WITH_STB_PROVIDER
        // P15: without a texture provider, gltfio logs "Missing texture
        // provider for image/png" and textured materials render black.
        // The stb decoder ships in the Filament prebuilt package (libstb.a).
        _textureProvider = filament::gltfio::createStbProvider(engine);
        _resourceLoader->addTextureProvider("image/png", _textureProvider);
        _resourceLoader->addTextureProvider("image/jpeg", _textureProvider);
#endif
#ifdef TILES_WITH_KTX2_LIBKTX
        // P25: KTX2 via our own provider backed by cesium-native's libktx
        // (see src/ktx2_libktx_provider.cpp for why filament's
        // createKtx2Provider cannot be used on cesium builds: basist::
        // symbol collision between Filament's and libktx's bundled basisu).
        _ktx2Provider = tiles::createLibktxKtx2Provider(engine);
        _resourceLoader->addTextureProvider("image/ktx2", _ktx2Provider);
#elif defined(TILES_WITH_KTX2_PROVIDER)
        // P25: KTX2 (KHR_texture_basisu) textures go through gltfio's
        // createKtx2Provider (ktxreader + basisu transcoder, both in the
        // Filament prebuilt package). Without it, image/ktx2 logs
        // "Missing texture provider" and renders black — the same
        // silent-black mode P15 fixed for PNG/JPEG.
        _ktx2Provider = filament::gltfio::createKtx2Provider(engine);
        _resourceLoader->addTextureProvider("image/ktx2", _ktx2Provider);
#endif
    }

    ~FilamentPrepareResources() override {
        // All tile assets must have been freed via free() before this runs
        // (Tileset destruction unloads tiles first).
        _materialProvider->destroyMaterials();
        delete _materialProvider;
        filament::gltfio::AssetLoader::destroy(&_assetLoader);
        delete _resourceLoader;
#ifdef TILES_WITH_STB_PROVIDER
        delete _textureProvider;
#endif
#if defined(TILES_WITH_KTX2_PROVIDER) || defined(TILES_WITH_KTX2_LIBKTX)
        delete _ktx2Provider;
#endif
    }

    CesiumAsync::Future<
        Cesium3DTilesSelection::TileLoadResultAndRenderResources>
    prepareInLoadThread(
        const CesiumAsync::AsyncSystem& asyncSystem,
        Cesium3DTilesSelection::TileLoadResult&& tileLoadResult,
        const glm::dmat4& /*transform*/,
        const std::any& /*rendererOptions*/) override {
        LoadThreadData* pData = nullptr;
        auto& result = tileLoadResult;

        // T1 (tilesetio): move the Model directly, no GLB serialization.
        // Case A: cesium-native already converted the content to a
        // CesiumGltf::Model (b3dm / i3dm / ... via GltfConverters).
        if (auto* pModel =
                std::get_if<CesiumGltf::Model>(&result.contentKind);
            pModel != nullptr) {
            pData = new LoadThreadData();
            pData->model = std::move(*pModel);
            // T1: extract RTC_CENTER from CESIUM_RTC extension (b3dm/i3dm).
            // The converter stores it here; tile.getTransform() is identity
            // for these formats. See old modelToGlb() for original logic.
            if (const auto* pRtc = pData->model->getExtension<
                    CesiumGltf::ExtensionCesiumRTC>();
                pRtc != nullptr && pRtc->center.size() == 3) {
                pData->rtcCenter = glm::dvec3(
                    pRtc->center[0], pRtc->center[1], pRtc->center[2]);
            }
            // P37: i3dm (EXT_mesh_gpu_instancing) needs two fixes:
            // 1. Expand instances to plain nodes (Filament's gltfio doesn't
            //    implement the extension, and our tilesetio backend reads
            //    plain nodes).
            // 2. Apply upAxisToZUp at the asset root to cancel the
            //    conjugation the i3dm converter applies to instance
            //    transforms.
            bool hasInstancing = false;
            bool hasNonInstancedMesh = false;
            for (const auto& node : pData->model->nodes) {
                if (node.getExtension<CesiumGltf::
                                        ExtensionExtMeshGpuInstancing>() !=
                    nullptr) {
                    hasInstancing = true;
                } else if (node.mesh >= 0) {
                    hasNonInstancedMesh = true;
                }
            }
            if (hasInstancing) {
                // P37: cmpt merges b3dm+i3dm into one Model. The b3dm part
                // must NOT get the i3dm upAxisFix. Detect merge by presence
                // of non-instanced meshes alongside instanced nodes.
                const bool isCmptMerge =
                    hasInstancing && hasNonInstancedMesh;
                if (!isCmptMerge) {
                    pData->upAxisFix = upAxisToZUp(pData->model.value());
                }
                // P37: mark model so convertModel skips its own upAxisFix
                // (already applied via pData->upAxisFix at asset root,
                // or not needed for cmpt merges).
                // TODO(P37): cmpt merge i3dm instances have coordinate space
                // issues (SSIM 0.4855). The instance translations may be in
                // a different space than the node transform. Needs deeper
                // investigation of CmptToGltfConverter output.
                pData->model->extras["tilesetio_i3dmFixApplied"] =
                    CesiumUtility::JsonValue(true);
                expandGpuInstancing(pData->model.value());
            }
        }

        // Case B: raw glb bytes — parse into Model via GltfReader.
        // (TODO T1: implement GltfReader parsing; for now skip raw GLB.)
        // The old code copied raw GLB bytes for gltfio; tilesetio needs a Model.

        Cesium3DTilesSelection::TileLoadResultAndRenderResources out;
        out.result = std::move(tileLoadResult);
        out.pRenderResources = pData;
        return asyncSystem.createResolvedFuture<
            Cesium3DTilesSelection::TileLoadResultAndRenderResources>(
            std::move(out));
    }

    // P37-B: strip EXT_structural_metadata / EXT_mesh_features texture
    // extensions from GLB before gltfio sees it. Filament v1.77's
    // ResourceLoader::createTextures() segfaults (strlen on null) when it
    // encounters property/feature-ID textures. We don't use metadata for
    // rendering, so stripping is safe: geometry/materials remain intact.
    static std::vector<std::uint8_t> stripMetadataTextureExtensions(
        const std::vector<std::uint8_t>& glb) {
        // Minimal GLB parse: header (12B) + JSON chunk.
        if (glb.size() < 20) return glb;
        const auto* p = glb.data();
        if (p[0] != 'g' || p[1] != 'l' || p[2] != 'T' || p[3] != 'F') return glb;
        std::uint32_t jsonLen;
        std::memcpy(&jsonLen, p + 12, 4);
        if (glb.size() < 20 + jsonLen) return glb;
        // Chunk type must be JSON (0x4E4F534A).
        std::uint32_t chunkType;
        std::memcpy(&chunkType, p + 16, 4);
        if (chunkType != 0x4E4F534A) return glb;

        rapidjson::Document doc;
        // Use insitu parsing on a copy (rapidjson needs mutable buffer).
        std::string jsonStr(reinterpret_cast<const char*>(p + 20), jsonLen);
        // P37-B debug: check if extensions exist before parsing.
        const bool hasStructural = jsonStr.find("EXT_structural_metadata") != std::string::npos;
        const bool hasMeshFeatures = jsonStr.find("EXT_mesh_features") != std::string::npos;
        if (hasStructural || hasMeshFeatures) {
            std::cerr << "[tiles_renderer] stripMetadata: found "
                      << (hasStructural ? "EXT_structural_metadata " : "")
                      << (hasMeshFeatures ? "EXT_mesh_features" : "")
                      << " in GLB JSON" << std::endl;
        }
        doc.ParseInsitu(jsonStr.data());
        if (doc.HasParseError() || !doc.IsObject()) return glb;

        bool modified = false;
        bool stripTextures = false;
        // Remove from extensionsUsed / extensionsRequired.
        for (const char* key : {"extensionsUsed", "extensionsRequired"}) {
            if (doc.HasMember(key) && doc[key].IsArray()) {
                auto& arr = doc[key];
                for (auto it = arr.Begin(); it != arr.End();) {
                    if (it->IsString() &&
                        (std::strcmp(it->GetString(), "EXT_structural_metadata") == 0 ||
                         std::strcmp(it->GetString(), "EXT_mesh_features") == 0)) {
                        it = arr.Erase(it);
                        modified = true;
                    } else {
                        ++it;
                    }
                }
            }
        }
        // Remove top-level extensions entries.
        if (doc.HasMember("extensions") && doc["extensions"].IsObject()) {
            auto& ext = doc["extensions"];
            if (ext.HasMember("EXT_structural_metadata")) {
                ext.RemoveMember("EXT_structural_metadata");
                modified = true;
            }
            if (ext.HasMember("EXT_mesh_features")) {
                ext.RemoveMember("EXT_mesh_features");
                modified = true;
            }
        }
        // Remove per-primitive / per-texture extension references.
        // (mesh.primitives[].extensions, textures[].extensions, etc.)
        std::function<void(rapidjson::Value&)> stripRecursive =
            [&](rapidjson::Value& v) {
                if (v.IsObject()) {
                    if (v.HasMember("extensions") && v["extensions"].IsObject()) {
                        auto& e = v["extensions"];
                        if (e.HasMember("EXT_structural_metadata")) {
                            e.RemoveMember("EXT_structural_metadata");
                            modified = true;
                        }
                        if (e.HasMember("EXT_mesh_features")) {
                            e.RemoveMember("EXT_mesh_features");
                            modified = true;
                        }
                    }
                    for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
                        stripRecursive(it->value);
                    }
                } else if (v.IsArray()) {
                    for (auto& elem : v.GetArray()) {
                        stripRecursive(elem);
                    }
                }
            };
        stripRecursive(doc);

        // P37-B: decide if textures must go. Triggers:
        // (1) metadata extensions stripped above, or
        // (2) images with external URIs (cesium-native's modelToGlb embeds
        //     buffers but external image files become dangling URIs that
        //     crash gltfio's createTextures with strlen(nullptr)).
        if (modified) stripTextures = true;
        if (doc.HasMember("images") && doc["images"].IsArray()) {
            for (auto& img : doc["images"].GetArray()) {
                if (img.IsObject() && img.HasMember("uri")) {
                    stripTextures = true;
                    std::cerr << "[tiles_renderer] stripMetadata: external "
                                 "image URI found, stripping textures"
                              << std::endl;
                    break;
                }
            }
            // P37-B debug: log image structure when not stripping.
            if (!stripTextures) {
                std::cerr << "[tiles_renderer] stripMetadata: images present "
                             "but no URI, count="
                          << doc["images"].Size() << std::endl;
            }
        }

        if (!modified && !stripTextures) return glb;

        // P37-B: if we stripped metadata extensions, also remove all
        // textures/images/samplers. The property/feature-ID textures are
        // what crash gltfio's createTextures(); materials fall back to
        // untextured rendering which is fine for conformance (we test
        // geometry loading, not metadata visualization).
        if (stripTextures) {
            if (doc.HasMember("textures")) {
                doc.RemoveMember("textures");
            }
            if (doc.HasMember("images")) {
                doc.RemoveMember("images");
            }
            if (doc.HasMember("samplers")) {
                doc.RemoveMember("samplers");
            }
            modified = true;
        }
        // Also clear texture references from materials (only when stripping).
        if (stripTextures && doc.HasMember("materials") &&
            doc["materials"].IsArray()) {
            for (auto& mat : doc["materials"].GetArray()) {
                if (!mat.IsObject()) continue;
                // pbrMetallicRoughness.baseColorTexture etc.
                std::function<void(rapidjson::Value&)> clearTexRefs =
                    [&](rapidjson::Value& v) {
                        if (v.IsObject()) {
                            // Remove any "*Texture" member that is an object
                            // with "index" (i.e., a texture reference).
                            for (auto it = v.MemberBegin(); it != v.MemberEnd();) {
                                const char* name = it->name.GetString();
                                size_t len = std::strlen(name);
                                if (len > 7 &&
                                    std::strcmp(name + len - 7, "Texture") == 0 &&
                                    it->value.IsObject()) {
                                    it = v.EraseMember(it);
                                    modified = true;
                                } else {
                                    clearTexRefs(it->value);
                                    ++it;
                                }
                            }
                        } else if (v.IsArray()) {
                            for (auto& e : v.GetArray()) clearTexRefs(e);
                        }
                    };
                clearTexRefs(mat);
            }
        }

        if (!modified) return glb;

        // Re-serialize JSON and repack GLB.
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        doc.Accept(writer);
        std::string newJson = sb.GetString();
        // Pad to 4-byte alignment with spaces (JSON chunk padding).
        while (newJson.size() % 4 != 0) newJson.push_back(' ');

        std::vector<std::uint8_t> out;
        out.reserve(12 + 8 + newJson.size() + (glb.size() - 20 - jsonLen));
        // Header: magic, version, new total length (patched below).
        out.insert(out.end(), p, p + 12);
        // JSON chunk header: new length + type.
        std::uint32_t newJsonLen = static_cast<std::uint32_t>(newJson.size());
        out.insert(out.end(), reinterpret_cast<std::uint8_t*>(&newJsonLen),
                   reinterpret_cast<std::uint8_t*>(&newJsonLen) + 4);
        out.insert(out.end(), p + 16, p + 20); // chunk type (JSON)
        out.insert(out.end(), newJson.begin(), newJson.end());
        // Copy remaining chunks (BIN etc.) as-is.
        out.insert(out.end(), p + 20 + jsonLen, p + glb.size());
        // Patch total length.
        std::uint32_t totalLen = static_cast<std::uint32_t>(out.size());
        std::memcpy(out.data() + 8, &totalLen, 4);
        return out;
    }

    void* prepareInMainThread(
        Cesium3DTilesSelection::Tile& tile, void* pLoadThreadResult) override {
        auto* pLoad =
            static_cast<LoadThreadData*>(pLoadThreadResult);
        if (pLoad == nullptr || !pLoad->model.has_value()) {
            delete pLoad;
            return nullptr; // not model content (or load failed)
        }

        // T1 (tilesetio): convert Model -> neutral RenderData -> Filament.
        // No GLB serialization, no gltfio AssetLoader.
        const glm::dvec3 rtcCenter = pLoad->rtcCenter;
        const glm::dmat4 upAxisFix = pLoad->upAxisFix;

        // Compute tile transform (double precision): 
        //   worldT = tile.getTransform() * translate(rtcCenter) * upAxisFix
        //   then apply modelMatrix and subtract localOrigin (P5 rebase).
        glm::dmat4 worldT = tile.getTransform();
        if (rtcCenter != glm::dvec3(0.0)) {
            glm::dmat4 rtcT(1.0);
            rtcT[3][0] = rtcCenter.x;
            rtcT[3][1] = rtcCenter.y;
            rtcT[3][2] = rtcCenter.z;
            worldT = worldT * rtcT;
        }
        worldT = worldT * upAxisFix;
        worldT = _modelMatrix * worldT;
        worldT[3][0] -= _localOrigin.x;
        worldT[3][1] -= _localOrigin.y;
        worldT[3][2] -= _localOrigin.z;

        double tileTransform[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                tileTransform[c * 4 + r] = worldT[c][r];

        // Convert Model to neutral render data.
        tilesetio::TileRenderData renderData =
            tilesetio::convertModel(
                pLoad->model.value(), tileTransform,
                Cesium3DTilesSelection::TileIdUtilities::createTileIdString(
                    tile.getTileID()));
        delete pLoad;

        if (renderData.primitives.empty()) {
            std::cerr << "[tiles_renderer] prepareInMainThread: tilesetio "
                         "convertModel produced no primitives"
                      << std::endl;
            return nullptr;
        }

        // Create Filament resources via backend (pure Filament, no gltfio).
        // The backend owns the shared unlit material.
        auto* pData = new TileRenderData();
        // Store tile transform for applying to entities when adding to scene.
        std::memcpy(
            pData->tileTransform, renderData.tileTransform,
            16 * sizeof(double));
        pData->filamentResources = _filamentBackend.createTile(
            _engine, renderData);
        if (pData->filamentResources.entities.empty()) {
            std::cerr << "[tiles_renderer] prepareInMainThread: tilesetio "
                         "FilamentBackend produced no entities"
                      << std::endl;
            delete pData;
            return nullptr;
        }

        // Keep transform pieces for setModelMatrix() recomposition (P33).
        pData->rtcCenter = rtcCenter;
        pData->upAxisFix = upAxisFix;
        return pData;
    }

    // P33: compose a tile's Filament render transform in double precision:
    //   render = modelMatrix * tileTransform * translate(rtcCenter)
    //            * upAxisFix - localOrigin
    // The b3dm RTC_CENTER (if any) is applied here in double precision
    // instead of being baked into the float glTF node, and localOrigin
    // (P5 rebase) keeps huge ECEF-style coordinates renderable in
    // float32. upAxisFix cancels the up-axis conjugation the i3dm
    // converter applies to instance transforms (identity for b3dm /
    // raw glb). modelMatrix (P33, default identity) transforms the whole
    // tileset in world space; localOrigin stays FIXED (it is a pure
    // float32-precision device, not part of the user transform), so the
    // tileset visibly moves relative to the orbit camera target — the
    // cesium.js behavior.
    // Shared by prepareInMainThread (new tiles) and
    // TilesetRenderer::Impl::setModelMatrix (already-loaded tiles).
    static filament::math::mat4f composeRenderTransform(
        const Cesium3DTilesSelection::Tile& tile,
        const glm::dvec3& rtcCenter, const glm::dmat4& upAxisFix,
        const glm::dmat4& modelMatrix, const glm::dvec3& localOrigin) {
        glm::dmat4 worldT = tile.getTransform();
        if (rtcCenter != glm::dvec3(0.0)) {
            glm::dmat4 rtcT(1.0);
            rtcT[3][0] = rtcCenter.x;
            rtcT[3][1] = rtcCenter.y;
            rtcT[3][2] = rtcCenter.z;
            worldT = worldT * rtcT;
        }
        worldT = worldT * upAxisFix;
        worldT = modelMatrix * worldT;
        // P33: localOrigin is a fixed float32-precision device — it does
        // NOT move with modelMatrix, so the user transform visibly moves
        // the tileset relative to the orbit camera target.
        worldT[3] -= glm::dvec4(localOrigin, 0.0);
        filament::math::mat4f m;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                m[c][r] = static_cast<float>(worldT[c][r]);
            }
        }
        return m;
    }

    // P5 rebase origin (world coordinates, double). Set once per tileset
    // after the root tile loads; defaults to (0,0,0) = no rebase.
    void setLocalOrigin(const glm::dvec3& origin) { _localOrigin = origin; }

    // P33: whole-tileset model matrix (world space, double). Applied to
    // tiles prepared after the call; already-loaded tiles are re-applied
    // by TilesetRenderer::Impl::setModelMatrix. Render thread only.
    void setModelMatrix(const glm::dmat4& matrix) { _modelMatrix = matrix; }
    const glm::dmat4& modelMatrix() const { return _modelMatrix; }
    // P36: read access for debug volume transforms (Impl::createVolumeEntity).
    const glm::dvec3& localOrigin() const { return _localOrigin; }

    void free(
        Cesium3DTilesSelection::Tile& /*tile*/, void* pLoadThreadResult,
        void* pMainThreadResult) noexcept override {
        // Case 1: prepareInMainThread never ran — drop the load-thread data.
        delete static_cast<LoadThreadData*>(pLoadThreadResult);
        // Case 2: full render data — remove from scene, destroy resources.
        auto* pData = static_cast<TileRenderData*>(pMainThreadResult);
        if (pData == nullptr) {
            return;
        }
        // T1 (tilesetio): destroy via backend (removes entities from scene).
        _filamentBackend.destroyTile(
            _engine, _scene, pData->filamentResources);
        pData->inScene = false;
        // P36: destroy debug volume entities (tile BV / request volume).
        if (pData->bvInScene) {
            _scene->removeEntities(&pData->bvEntity, 1);
            _engine->destroy(pData->bvEntity);
            pData->bvInScene = false;
        }
        if (pData->rqInScene) {
            _scene->removeEntities(&pData->rqEntity, 1);
            _engine->destroy(pData->rqEntity);
            pData->rqInScene = false;
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
    // T1 (tilesetio): backend for direct Model -> Filament conversion.
    // The gltfio AssetLoader/ResourceLoader above are legacy and will be
    // removed in T5 once tilesetio is fully validated.
    tilesetio::FilamentBackend _filamentBackend;
#ifdef TILES_WITH_STB_PROVIDER
    // P15: stb image decoder feeding gltfio's ResourceLoader (PNG/JPEG).
    // Must outlive _resourceLoader; destroyed after it above.
    filament::gltfio::TextureProvider* _textureProvider = nullptr;
#endif
#if defined(TILES_WITH_KTX2_PROVIDER) || defined(TILES_WITH_KTX2_LIBKTX)
    // P25: KTX2 (KHR_texture_basisu) decoder feeding gltfio's
    // ResourceLoader. Same lifetime rule as _textureProvider above.
    filament::gltfio::TextureProvider* _ktx2Provider = nullptr;
#endif
    // P5 rebase origin (world coordinates, double); subtracted from every
    // tile translation in double precision before the float32 conversion.
    glm::dvec3 _localOrigin{0.0, 0.0, 0.0};
    // P33: whole-tileset model matrix (world space, double); identity =
    // no user transform.
    glm::dmat4 _modelMatrix{1.0};
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

    // P36: destroy shared debug line resources (entities are owned per
    // tile and die in free()).
    ~Impl() {
        // P36: remove all debug volume entities BEFORE destroying the
        // shared line-box resources: the entities hold MaterialInstances
        // that Filament refuses to destroy while still referenced by a
        // Renderable. Clearing the flags and re-applying walks the tree
        // and removes every bvEntity/rqEntity (tileset still alive here;
        // member destruction runs after the destructor body).
        debugShowBoundingVolume = false;
        debugShowContentBoundingVolume = false;
        debugShowViewerRequestVolume = false;
        updateDebugVolumeVisibility();
        if (engine != nullptr) {
            if (debugLineVb != nullptr) {
                engine->destroy(debugLineVb);
            }
            if (debugLineIb != nullptr) {
                engine->destroy(debugLineIb);
            }
            if (debugLineMaterialBv != nullptr) {
                engine->destroy(debugLineMaterialBv);
            }
            if (debugLineMaterialRq != nullptr) {
                engine->destroy(debugLineMaterialRq);
            }
            if (debugLineMaterial != nullptr) {
                engine->destroy(debugLineMaterial);
            }
        }
    }

    // P19: pending cache budget; applied to TilesetOptions at construction
    // and live-mutated afterwards. -1 = cesium-native default (512MB).
    std::int64_t maxCachedBytes = -1;

    // P31: pending live SSE override from setMaximumScreenSpaceError();
    // applied at construction. hasPendingMaxSse tracks "explicitly set"
    // (a negative stash means "restore default", not "unset"); the stashed
    // value is already normalized (>= 0) at stash time.
    double pendingMaxSse = 16.0;
    bool hasPendingMaxSse = false;

    // P31: options the live tileset was constructed with (diagnostic).
    Renderer::TilesetOptions appliedOptions;

    bool loadTileset(const std::string& urlOrPath) {
        return loadTileset(urlOrPath, Renderer::TilesetOptions{});
    }

    bool loadTileset(const std::string& urlOrPath,
                     const Renderer::TilesetOptions& sdkOptions) {
        lastError.clear();
        // P32: reset the per-tileset event state for the new load. (A
        // loadTileset() that replaces a live tileset fires tileUnload for
        // the old tileset's tiles from Renderer::loadTileset, before the
        // old TilesetRenderer is destroyed — see renderer.cpp.)
        tileStates.clear();
        initialTilesLoadedFired = false;
        lastProgressPending = -1;
        lastProgressProcessing = -1;
        lastTilesLoaded = false;
        // P33: the update clock restarts for the new tileset. (show /
        // preloadWhenHidden / modelMatrix are re-applied to the fresh
        // TilesetRenderer by Renderer::loadTileset from its stash — see
        // renderer.cpp.)
        hasFirstUpdate = false;
        loadTime = std::chrono::steady_clock::now();
        if (loadFailureQueue) {
            std::lock_guard<std::mutex> lock(loadFailureQueue->mutex);
            loadFailureQueue->pending.clear();
        }
        // P31: validate/normalize the SDK options before touching the
        // filesystem or network. Invalid values fail fast (the live
        // tileset, if any, is untouched — same as any other failed load).
        Renderer::TilesetOptions eff = sdkOptions;
        {
            // Negative, NaN, or +inf SSE restores the default 16 — same
            // semantics as the live setMaximumScreenSpaceError().
            if (!(eff.maximumScreenSpaceError >= 0.0) ||
                !std::isfinite(eff.maximumScreenSpaceError)) {
                eff.maximumScreenSpaceError = 16.0;
            }
            // 0 simultaneous loads would make cesium-native's load pump
            // exit early forever (no tile ever loads); 0 is meaningless,
            // so restore the default.
            if (eff.maximumSimultaneousTileLoads == 0) {
                eff.maximumSimultaneousTileLoads = 20;
            }
            if (eff.loadingDescendantLimit == 0) {
                eff.loadingDescendantLimit = 20;
            }
            // lodTransitionLength feeds a division (deltaTime / length);
            // non-positive or NaN is meaningless.
            if (!(eff.lodTransitionLength > 0.0f)) {
                eff.lodTransitionLength = 1.0f;
            }
            // The ellipsoid feeds real transforms; silently replacing a
            // user's bogus radii with WGS84 would be dishonest, so fail.
            for (int i = 0; i < 3; ++i) {
                const double r = eff.ellipsoidRadii[i];
                if (!(r > 0.0) || !std::isfinite(r)) {
                    lastError = "invalid ellipsoidRadii: all three radii "
                                "must be finite and > 0";
                    std::cerr << "[tiles_renderer] loadTileset: " << lastError
                              << std::endl;
                    return false;
                }
            }
        }
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
                lastError = "file not found: " + urlOrPath;
                std::cerr << "[tiles_renderer] loadTileset: " << lastError
                          << std::endl;
                return false;
            }
            // ifstream opens directories successfully on POSIX; reading one
            // as a tileset crashes downstream in the asset pipeline. Fail
            // fast instead (same class of bug as P23's corrupt-tileset
            // fail-fast).
            std::error_code ec;
            if (!std::filesystem::is_regular_file(localPath, ec) || ec) {
                lastError = "not a file: " + urlOrPath;
                std::cerr << "[tiles_renderer] loadTileset: " << lastError
                          << std::endl;
                return false;
            }
        }
        // Register cesium-native's tile content converters (glTF, b3dm, etc.).
        // Without this, GLB magic bytes are not recognized and tile loads
        // fail. Must be called once before any Tileset is created.
        // P6: std::call_once instead of a bool flag (thread-safe; the old
        // flag raced if two tilesets loaded concurrently on first use).
        static std::once_flag contentTypesRegisteredFlag;
        std::call_once(contentTypesRegisteredFlag, []() {
            Cesium3DTilesContent::registerAllTileContentTypes();
            // P28: wrap the magic-dispatched converters so a converter
            // result with an empty model and warnings-only errors becomes
            // a hard error (graceful tile failure) instead of the
            // unchecked `*result.model` dereference in TilesetJsonLoader
            // (UB, SIGSEGV in practice; upstream #1457).
            tiles::registerHardenedContentConverters();
        });

        auto pAccessor = std::make_shared<RoutingAssetAccessor>();
        auto pPrepare =
            std::make_shared<FilamentPrepareResources>(engine, scene);
        // Keep an AsyncSystem handle: TilesetExternals takes a copy (it is a
        // shared-ownership wrapper), we keep ours for pumping main-thread
        // tasks every frame.
        // P22: build the replacement tileset with LOCAL state first and
        // commit to members only after its root tile arrives. A failed load
        // (bad URL, corrupt tileset.json, 30s timeout) must leave the
        // currently-loaded tileset untouched — the old code replaced the
        // members up front, so a failed second loadTileset() destroyed the
        // working tileset and left loaded=true with tileset=nullptr.
        auto newTaskProcessor = std::make_shared<SimpleTaskProcessor>();
        auto newAsyncSystem =
            std::make_optional<CesiumAsync::AsyncSystem>(newTaskProcessor);
        // P23: fail-fast pre-flight — validate the root tileset.json BEFORE
        // constructing the Tileset, so a corrupt-but-present document fails
        // in milliseconds instead of spinning the 30s root-wait below. A
        // failed pre-flight changes nothing: the live tileset (if any) is
        // untouched, exactly like any other failed load (P22).
        std::string preflightError;
        if (!preflightTilesetRoot(
                url, localPath, *pAccessor, *newAsyncSystem, preflightError)) {
            // No "loadTileset: " prefix here: Renderer::loadTileset adds it
            // (same convention as the "file not found" probe above).
            lastError = preflightError;
            std::cerr << "[tiles_renderer] loadTileset: " << lastError
                      << std::endl;
            return false;
        }
        Cesium3DTilesSelection::TilesetExternals externals{
            pAccessor, pPrepare, *newAsyncSystem,
            std::make_shared<CesiumUtility::CreditSystem>()};

        Cesium3DTilesSelection::TilesetOptions options;
        // P31: map the SDK-level options (cesium.js-aligned subset) onto
        // cesium-native. A pending setMaximumScreenSpaceError() wins over
        // the construction value, mirroring the setMaxCachedBytes pattern.
        // `eff` is the validated/normalized copy from above; the pending
        // value was normalized at stash time.
        if (hasPendingMaxSse) {
            eff.maximumScreenSpaceError = pendingMaxSse;
        }
        options.maximumScreenSpaceError = eff.maximumScreenSpaceError;
        options.forbidHoles = eff.forbidHoles;
        options.preloadAncestors = eff.preloadAncestors;
        options.preloadSiblings = eff.preloadSiblings;
        options.enableFrustumCulling = eff.enableFrustumCulling;
        options.enableFogCulling = eff.enableFogCulling;
        options.maximumSimultaneousTileLoads = eff.maximumSimultaneousTileLoads;
        options.loadingDescendantLimit = eff.loadingDescendantLimit;
        options.enableLodTransitionPeriod = eff.enableLodTransitionPeriod;
        options.lodTransitionLength = eff.lodTransitionLength;
        options.ellipsoid = CesiumGeospatial::Ellipsoid(glm::dvec3(
            eff.ellipsoidRadii[0], eff.ellipsoidRadii[1],
            eff.ellipsoidRadii[2]));
        // P32: tileset.json / layer.json / implicit-subtree load failures.
        // The callback is owned by the Tileset and may outlive the Impl
        // during teardown, so it only touches the shared failure queue —
        // never the Impl. Drained on the render thread in
        // dispatchFrameEvents(), which raises onTileFailed.
        if (!loadFailureQueue) {
            loadFailureQueue = std::make_shared<Impl::LoadFailureQueue>();
        }
        const std::shared_ptr<Impl::LoadFailureQueue> failureQueue =
            loadFailureQueue;
        options.loadErrorCallback =
            [failureQueue](
                const Cesium3DTilesSelection::TilesetLoadFailureDetails&
                    details) {
                Renderer::TileFailedInfo info;
                info.message = details.message;
                std::lock_guard<std::mutex> lock(failureQueue->mutex);
                failureQueue->pending.push_back(std::move(info));
            };
        // P19: honor a host-set cache budget (default: cesium-native's
        // 512MB). unloadCachedBytes() reads _options.maximumCachedBytes
        // every update, so this also stays live-mutable via
        // setMaxCachedBytes().
        if (maxCachedBytes > 0) {
            options.maximumCachedBytes = maxCachedBytes;
        }
        auto newTileset = std::make_unique<Cesium3DTilesSelection::Tileset>(
            externals, url, options);
        // Wait (bounded) for the root tile metadata so load() can report
        // success/failure honestly instead of always succeeding.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (newTileset->getRootTile() == nullptr &&
               std::chrono::steady_clock::now() < deadline) {
            newAsyncSystem->dispatchMainThreadTasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (newTileset->getRootTile() == nullptr) {
            lastError = "failed to load tileset (no root tile within 30s): " +
                        urlOrPath;
            std::cerr << "[tiles_renderer] loadTileset: " << lastError
                      << std::endl;
            // newTileset (and its accessor/prepare/async state) dies here;
            // the previous tileset, if any, is untouched and keeps
            // rendering.
            return false;
        }
        // Commit: replace the live state. Assigning `tileset` destroys the
        // old Tileset, whose destructor unloads its tiles (free() removes
        // their Filament scene nodes) while its externals still hold the
        // old prepare resources alive — so the scene is clean before the
        // new tileset's first updateViewGroup.
        taskProcessor = std::move(newTaskProcessor);
        asyncSystem = std::move(newAsyncSystem);
        // Keep the prepare resources alive as long as the tileset: the
        // Tileset only holds the shared_ptr from externals during
        // construction, so retain our own copy too.
        prepareResources = pPrepare;
        // P33: the model matrix may have been set before load (stash-then-
        // forward) — push it into the fresh prepare resources so tiles
        // prepared after this point use it.
        prepareResources->setModelMatrix(modelMatrix);
        tileset = std::move(newTileset);
        // P31: remember the effective options for currentTilesetOptions().
        appliedOptions = eff;
        // P34: cache tileset.json "extensionsUsed" for hasExtension().
        // getMetadata() is valid once the root tile is loaded (we waited
        // for it above); empty when the tileset declares none.
        extensionsUsed.clear();
        trimRequested = false;
        if (const auto* pMeta = tileset->getMetadata()) {
            extensionsUsed = pMeta->extensionsUsed;
        }
        // P5 rebase: pick the tileset's world-space center as the local
        // origin so huge coordinates (e.g. ECEF) survive the float32 render
        // transform. For small local tilesets this is ~(0,0,0) = no-op.
        localOrigin = computeLocalOrigin(*tileset->getRootTile());
        hasOrigin = true;
        pPrepare->setLocalOrigin(localOrigin);
        std::cout << "[tiles_renderer] tileset loaded: " << urlOrPath
                  << " (local origin: " << localOrigin.x << ", "
                  << localOrigin.y << ", " << localOrigin.z << ")"
                  << std::endl;
        loaded = true;
        return true;
    }

    // World-space center of the root tile's bounding volume (double).
    // Used as the rebase origin; falls back to (0,0,0) with a warning for
    // volume types we don't know how to center.
    static glm::dvec3 computeLocalOrigin(
        const Cesium3DTilesSelection::Tile& root) {
        using namespace Cesium3DTilesSelection;
        const BoundingVolume& bv = root.getBoundingVolume();
        if (const auto* pBox =
                std::get_if<CesiumGeometry::OrientedBoundingBox>(&bv);
            pBox != nullptr) {
            return pBox->getCenter();
        }
        if (const auto* pSphere =
                std::get_if<CesiumGeometry::BoundingSphere>(&bv);
            pSphere != nullptr) {
            return pSphere->getCenter();
        }
        if (const auto* pRegion =
                std::get_if<CesiumGeospatial::BoundingRegion>(&bv);
            pRegion != nullptr) {
            return pRegion->getBoundingBox().getCenter();
        }
        if (const auto* pLoose = std::get_if<
                CesiumGeospatial::BoundingRegionWithLooseFittingHeights>(&bv);
            pLoose != nullptr) {
            return pLoose->getBoundingRegion().getBoundingBox().getCenter();
        }
        std::cerr << "[tiles_renderer] computeLocalOrigin: unsupported "
                     "bounding volume type; rebase disabled"
                  << std::endl;
        return glm::dvec3(0.0);
    }

    // P32: build the public event payload for a tile. The URL is best
    // effort: only tiles whose TileID is a string (external tileset
    // references) carry one; content URLs resolved by cesium-native's
    // loaders are not exposed on the Tile, so url is "" for the common
    // case. Documented as such in renderer.h.
    static Renderer::TileEventInfo tileEventInfo(
        const Cesium3DTilesSelection::Tile& tile) {
        Renderer::TileEventInfo info;
        info.tileId =
            Cesium3DTilesSelection::TileIdUtilities::createTileIdString(
                tile.getTileID());
        if (const std::string* pUrl =
                std::get_if<std::string>(&tile.getTileID());
            pUrl != nullptr) {
            info.url = *pUrl;
        }
        return info;
    }

    // P32: dispatch cesium.js-style tileset events. Runs on the render
    // thread at the end of updateTiles() (i.e. inside renderFrame()), after
    // the traversal. Order per frame: queued loadErrorCallback failures,
    // then state transitions (tileLoad/tileUnload/tileFailed, parent before
    // children), then tileVisible (render selection order), then
    // loadProgress (on change), allTilesLoaded (while fully loaded),
    // initialTilesLoaded (once per loadTileset).
    //
    // tileLoad/tileUnload/tileFailed come from TileLoadState transitions
    // observed in a single tree walk, which also yields the in-flight
    // content count for loadProgress/allTilesLoaded and for P33's
    // tilesLoaded(). The walk runs every frame (it is O(tiles) pointer
    // chasing with no per-tile allocation); only the transition bookkeeping
    // and the event payloads allocate.
    void dispatchFrameEvents(
        const Cesium3DTilesSelection::ViewUpdateResult& viewResult) {
        const Renderer::TilesetEventCallbacks& cb = eventCallbacks;

        // 1. Queued tileset.json-level failures (loadErrorCallback).
        if (cb.onTileFailed && loadFailureQueue) {
            std::vector<Renderer::TileFailedInfo> queued;
            {
                std::lock_guard<std::mutex> lock(loadFailureQueue->mutex);
                queued.swap(loadFailureQueue->pending);
            }
            for (const auto& info : queued) {
                cb.onTileFailed(info);
            }
        } else if (loadFailureQueue) {
            std::lock_guard<std::mutex> lock(loadFailureQueue->mutex);
            loadFailureQueue->pending.clear();
        }

        const bool wantTransitions =
            cb.onTileLoad || cb.onTileUnload || cb.onTileFailed;

        // 2. Single tree walk: transitions + in-flight count. Always runs
        // (P33 tilesLoaded); transition events only fire when observed.
        // Per-tile transition events are COLLECTED during the walk and
        // dispatched after it (see below): a callback must never observe
        // the tileStates map mid-walk, and the map is fully updated before
        // the first callback runs (ADR-0031 reentrancy rule).
        struct PendingEvent {
            enum class Kind { Load, Unload, Failed };
            Kind kind;
            Renderer::TileEventInfo info;    // Load / Unload
            Renderer::TileFailedInfo failed; // Failed
        };
        std::vector<PendingEvent> pendingEvents;
        std::int64_t inFlightContent = 0;
        if (tileset != nullptr) {
            const Cesium3DTilesSelection::Tile* pRoot =
                tileset->getRootTile();
            if (pRoot != nullptr) {
                std::unordered_set<const Cesium3DTilesSelection::Tile*> seen;
                std::vector<const Cesium3DTilesSelection::Tile*> stack{pRoot};
                using Cesium3DTilesSelection::TileLoadState;
                while (!stack.empty()) {
                    const auto* pTile = stack.back();
                    stack.pop_back();
                    seen.insert(pTile);
                    const TileLoadState state = pTile->getState();
                    auto it = tileStates.find(pTile);
                    const bool known = it != tileStates.end();
                    const TileLoadState prev =
                        known ? it->second.state : TileLoadState::Unloaded;
                    if (!known) {
                        // Materialize the ID string once per tile (event
                        // payloads + the pruned-tile unload sweep below).
                        TileStateRecord rec;
                        rec.id = Cesium3DTilesSelection::
                            TileIdUtilities::createTileIdString(
                                pTile->getTileID());
                        rec.state = state;
                        it = tileStates.emplace(pTile, std::move(rec)).first;
                    }
                    if (wantTransitions && prev != state) {
                        const bool wasDone = prev == TileLoadState::Done;
                        const bool isDone = state == TileLoadState::Done;
                        const bool wasFailed =
                            prev == TileLoadState::Failed ||
                            prev == TileLoadState::FailedTemporarily;
                        const bool isFailed =
                            state == TileLoadState::Failed ||
                            state == TileLoadState::FailedTemporarily;
                        if (isDone && !wasDone && cb.onTileLoad) {
                            Renderer::TileEventInfo info;
                            info.tileId = it->second.id;
                            if (const std::string* pUrl = std::get_if<
                                    std::string>(&pTile->getTileID());
                                pUrl != nullptr) {
                                info.url = *pUrl;
                            }
                            pendingEvents.push_back(
                                {PendingEvent::Kind::Load, std::move(info), {}});
                        } else if (!isDone && wasDone && cb.onTileUnload) {
                            Renderer::TileEventInfo info;
                            info.tileId = it->second.id;
                            if (const std::string* pUrl = std::get_if<
                                    std::string>(&pTile->getTileID());
                                pUrl != nullptr) {
                                info.url = *pUrl;
                            }
                            pendingEvents.push_back(
                                {PendingEvent::Kind::Unload, std::move(info),
                                 {}});
                        } else if (isFailed && !wasFailed && cb.onTileFailed) {
                            Renderer::TileFailedInfo info;
                            info.tileId = it->second.id;
                            if (const std::string* pUrl = std::get_if<
                                    std::string>(&pTile->getTileID());
                                pUrl != nullptr) {
                                info.url = *pUrl;
                            }
                            // Honest: cesium-native does not surface the
                            // underlying content error text on the Tile.
                            info.message = "tile content failed to load";
                            pendingEvents.push_back(
                                {PendingEvent::Kind::Failed, {}, std::move(info)});
                        }
                    }
                    it->second.state = state;
                    if (state == TileLoadState::ContentLoading ||
                        state == TileLoadState::ContentLoaded) {
                        ++inFlightContent;
                    }
                    for (const auto& child : pTile->getChildren()) {
                        stack.push_back(&child);
                    }
                }
                // Tiles pruned from the tree (implicit tiling): a tile that
                // was Done and vanished had its content released.
                for (auto it = tileStates.begin(); it != tileStates.end();) {
                    if (seen.find(it->first) == seen.end()) {
                        if (wantTransitions && cb.onTileUnload &&
                            it->second.state ==
                                Cesium3DTilesSelection::TileLoadState::Done) {
                            Renderer::TileEventInfo info;
                            info.tileId = it->second.id;
                            pendingEvents.push_back(
                                {PendingEvent::Kind::Unload, std::move(info),
                                 {}});
                        }
                        it = tileStates.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }

        // Dispatch the collected per-tile transitions AFTER the walk: the
        // tileStates map is fully updated, so a callback observes
        // consistent state (ADR-0031). Order matches collection order.
        for (const auto& ev : pendingEvents) {
            switch (ev.kind) {
            case PendingEvent::Kind::Load:
                cb.onTileLoad(ev.info);
                break;
            case PendingEvent::Kind::Unload:
                cb.onTileUnload(ev.info);
                break;
            case PendingEvent::Kind::Failed:
                cb.onTileFailed(ev.failed);
                break;
            }
        }

        // P33: the fully-loaded verdict, queryable via tilesLoaded()
        // without event callbacks.
        const std::int64_t pending = static_cast<std::int64_t>(lastWorkerQueue) +
                                     static_cast<std::int64_t>(lastMainQueue);
        lastTilesLoaded = (pending <= 0 && inFlightContent <= 0);

        // 3. tileVisible: the traversal's render selection, in order.
        if (cb.onTileVisible) {
            for (const auto& pTile : viewResult.tilesToRenderThisFrame) {
                cb.onTileVisible(tileEventInfo(*pTile));
            }
        }

        // 4. loadProgress / allTilesLoaded / initialTilesLoaded.
        const bool wantProgress = wantTransitions || cb.onLoadProgress ||
                                  cb.onAllTilesLoaded ||
                                  cb.onInitialTilesLoaded;
        if (wantProgress) {
            if (cb.onLoadProgress &&
                (pending != lastProgressPending ||
                 inFlightContent != lastProgressProcessing)) {
                lastProgressPending = pending;
                lastProgressProcessing = inFlightContent;
                cb.onLoadProgress(pending, inFlightContent);
            }
            if (lastTilesLoaded && cb.onAllTilesLoaded) {
                cb.onAllTilesLoaded();
            }
            if (lastTilesLoaded && !initialTilesLoadedFired &&
                cb.onInitialTilesLoaded) {
                initialTilesLoadedFired = true;
                cb.onInitialTilesLoaded();
            }
        }
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

        // P33: first successful frame after load — starts the
        // timeSinceLoadMs() clock.
        if (!hasFirstUpdate) {
            hasFirstUpdate = true;
            firstUpdateTime = std::chrono::steady_clock::now();
        }

        // P33: show=false hides the tileset (cesium.js show /
        // preloadWhenHidden). Without preloadWhenHidden the traversal is
        // skipped entirely — tiles neither load nor render and the tileset
        // is frozen. With preloadWhenHidden, tiles keep loading (events
        // still fire) but nothing is ever added to the Filament scene.
        const bool hidden = !show;
        const bool doTraversal = !hidden || preloadWhenHidden;

        // The traversal's explicit render selection drives Scene visibility
        // (not tile.isRenderable(), which is true for every loaded tile).
        // Include fading-out tiles so LOD transitions don't pop. When
        // hidden, the set stays empty so the scene is swept clean.
        std::unordered_set<Cesium3DTilesSelection::Tile::ConstPointer>
            wantVisible;
        if (doTraversal) {
            // Orbit camera -> cesium ViewState. The orbit target is the
            // tileset's rebase origin (double precision); rendering shows
            // the same tiles rebased around (0,0,0) — see
            // FilamentPrepareResources.
            const double yaw = cam.yawDegrees * kPi / 180.0;
            const double pitch = cam.pitchDegrees * kPi / 180.0;
            const glm::dvec3 target =
                hasOrigin ? localOrigin
                          : glm::dvec3(cam.targetX, cam.targetY, cam.targetZ);
            const glm::dvec3 eye(
                target.x + cam.distance * std::cos(pitch) * std::sin(yaw),
                target.y + cam.distance * std::sin(pitch),
                target.z + cam.distance * std::cos(pitch) * std::cos(yaw));
            const glm::dvec3 direction = glm::normalize(target - eye);
            const glm::dvec3 up(0.0, 1.0, 0.0);
            constexpr double kVfov = 45.0 * kPi / 180.0; // matches SDK camera
            const double aspect = viewportWidth / viewportHeight;
            const double kHfov =
                2.0 * std::atan(std::tan(kVfov / 2.0) * aspect);
            Cesium3DTilesSelection::ViewState viewState(
                eye, direction, up, glm::dvec2(viewportWidth, viewportHeight),
                kHfov, kVfov);
            const Cesium3DTilesSelection::ViewUpdateResult& viewResult =
                tileset->updateViewGroup(
                    tileset->getDefaultViewGroup(), {viewState});
            // updateViewGroup only fills the traversal's load queue;
            // loadTiles() actually starts/processes the queued tile content
            // loads.
            // P34: trimLoadedTiles() — briefly zero the cache budget so
            // loadTiles()'s internal unloadCachedBytes(0, 0.0) evicts
            // everything not in use (tileCacheUnloadTimeLimit defaults to
            // 0.0 = no time limit, so one pass clears it), then restore.
            // Tiles in use are never unloaded (cesium-native guarantee);
            // evictions fire tileUnload via the P32 event path. The RAII
            // guard restores the budget even if loadTiles() throws.
            if (trimRequested) {
                trimRequested = false;
                auto& opts = tileset->getOptions();
                const std::int64_t saved = opts.maximumCachedBytes;
                struct BudgetRestore {
                    Cesium3DTilesSelection::TilesetOptions& opts;
                    std::int64_t saved;
                    ~BudgetRestore() { opts.maximumCachedBytes = saved; }
                } guard{opts, saved};
                opts.maximumCachedBytes = 0;
                tileset->loadTiles();
            } else {
                tileset->loadTiles();
            }

            // P17: capture the traversal's diagnostics for tileStats().
            lastSelected =
                static_cast<int>(viewResult.tilesToRenderThisFrame.size());
            lastWorkerQueue = viewResult.workerThreadTileLoadQueueLength;
            lastMainQueue = viewResult.mainThreadTileLoadQueueLength;
            // P20: per-tile identity of the render selection (diagnostic).
            lastSelectedIds.clear();
            lastSelectedIds.reserve(viewResult.tilesToRenderThisFrame.size());
            for (const auto& pTile : viewResult.tilesToRenderThisFrame) {
                lastSelectedIds.push_back(
                    Cesium3DTilesSelection::TileIdUtilities::
                        createTileIdString(pTile->getTileID()));
            }

            // P32: cesium.js-style tileset events, dispatched on the render
            // thread at the end of the traversal.
            dispatchFrameEvents(viewResult);

            if (!hidden) {
                for (const auto& pTile : viewResult.tilesToRenderThisFrame) {
                    wantVisible.insert(pTile);
                }
                for (const auto& pTile : viewResult.tilesFadingOut) {
                    wantVisible.insert(pTile);
                }
            }
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
            // T1 (tilesetio): use backend entities instead of gltfio asset.
            if (pData != nullptr &&
                !pData->filamentResources.entities.empty()) {
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
                const auto& entities = pData->filamentResources.entities;
                if (want && !pData->inScene) {
                    // T1 (tilesetio): apply tile transform to each entity.
                    {
                        auto& tm = engine->getTransformManager();
                        filament::math::mat4f m;
                        for (int c = 0; c < 4; ++c)
                            for (int r = 0; r < 4; ++r)
                                m[c][r] = static_cast<float>(
                                    pData->tileTransform[c * 4 + r]);
                        for (auto e : entities) {
                            tm.setTransform(tm.getInstance(e), m);
                        }
                    }
                    scene->addEntities(
                        entities.data(),
                        entities.size());
                    pData->inScene = true;
                    // P35: debugShowUrl logs the tile ID as it becomes
                    // visible (no on-screen text renderer in this SDK).
                    if (debugShowUrl) {
                        std::cerr << "[tiles_renderer] debugShowUrl: "
                                     "visible tile "
                                  << Cesium3DTilesSelection::
                                         TileIdUtilities::
                                             createTileIdString(
                                                 tile.getTileID())
                                  << std::endl;
                    }
                } else if (!want && pData->inScene) {
                    scene->removeEntities(
                        entities.data(),
                        entities.size());
                    pData->inScene = false;
                }
                // P35: keep the debug wireframe in sync with the tile's
                // scene membership and the debug flags (P36: content
                // wireframe + tile BV + request volume).
                updateTileWireframe(tile, *pData);
                updateTileBoundingVolume(tile, *pData);
                updateTileRequestVolume(tile, *pData);
                if (pData->inScene) {
                    ++renderedCount;
                }
            }
        }
        for (Cesium3DTilesSelection::Tile& child : tile.getChildren()) {
            updateTileVisibility(child, wantVisible);
        }
    }

    // P36: debug line-box resources for the tile/request volume overlays.
    // One shared unit-box (±1) LINES geometry; each tile's bvEntity /
    // rqEntity carries its volume in the entity transform, so toggling is
    // add/remove-entity only. Render thread only; built lazily.
    void ensureDebugLineResources() {
        if (debugLineMaterial != nullptr) {
            return;
        }
        debugLineMaterial = filament::Material::Builder()
                                .package(unlit_color_filamat,
                                         unlit_color_filamat_len)
                                .build(*engine);
        debugLineMaterialBv = debugLineMaterial->createInstance();
        debugLineMaterialBv->setParameter(
            "color", filament::RgbType::LINEAR,
            filament::math::float3{1.0f, 1.0f, 0.0f}); // tile BV: yellow
        debugLineMaterialRq = debugLineMaterial->createInstance();
        debugLineMaterialRq->setParameter(
            "color", filament::RgbType::LINEAR,
            filament::math::float3{0.0f, 1.0f, 1.0f}); // request vol: cyan
        // Unit box corners at ±1; 12 edges as 24 indices.
        static const filament::math::float3 kBoxVerts[8] = {
            {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
            {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
        static const std::uint16_t kBoxIndices[24] = {
            0, 1, 1, 2, 2, 3, 3, 0, // bottom
            4, 5, 5, 6, 6, 7, 7, 4, // top
            0, 4, 1, 5, 2, 6, 3, 7  // sides
        };
        debugLineVb = filament::VertexBuffer::Builder()
                          .vertexCount(8)
                          .bufferCount(1)
                          .attribute(filament::VertexAttribute::POSITION, 0,
                                     filament::VertexBuffer::AttributeType::
                                         FLOAT3)
                          .build(*engine);
        debugLineVb->setBufferAt(
            *engine, 0,
            filament::VertexBuffer::BufferDescriptor(
                kBoxVerts, sizeof(kBoxVerts)));
        debugLineIb =
            filament::IndexBuffer::Builder()
                .indexCount(24)
                .bufferType(filament::IndexBuffer::IndexType::USHORT)
                .build(*engine);
        debugLineIb->setBuffer(
            *engine, filament::IndexBuffer::BufferDescriptor(
                         kBoxIndices, sizeof(kBoxIndices)));
    }

    // P36: entity transform for a debug volume box. The OBB is in tileset
    // space (cesium-native pre-applies tileTransform); render space is
    // modelMatrix * (tilesetSpace - localOrigin), mirroring
    // composeRenderTransform's P33 rule that localOrigin never moves with
    // modelMatrix.
    static filament::math::mat4f composeVolumeTransform(
        const glm::dvec3& center, const glm::dmat3& halfAxes,
        const glm::dmat4& modelMatrix, const glm::dvec3& localOrigin) {
        const glm::dmat3 rotScale(modelMatrix);
        const glm::dmat3 ha = rotScale * halfAxes;
        const glm::dvec4 tc =
            modelMatrix * glm::dvec4(center - localOrigin, 1.0);
        const glm::dmat4 m(
            glm::dvec4(ha[0], 0.0), glm::dvec4(ha[1], 0.0),
            glm::dvec4(ha[2], 0.0), glm::dvec4(tc.x, tc.y, tc.z, 1.0));
        filament::math::mat4f out;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                out[c][r] = static_cast<float>(m[c][r]);
            }
        }
        return out;
    }

    // P36: create one debug line-box entity for an OBB volume.
    utils::Entity createVolumeEntity(
        const CesiumGeometry::OrientedBoundingBox& obb,
        filament::MaterialInstance* materialInstance) {
        ensureDebugLineResources();
        utils::Entity e = utils::EntityManager::get().create();
        filament::RenderableManager::Builder(1)
            .boundingBox({{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}})
            .material(0, materialInstance)
            .geometry(0, filament::RenderableManager::PrimitiveType::LINES,
                      debugLineVb, debugLineIb, 0, 24)
            .culling(false)
            .receiveShadows(false)
            .castShadows(false)
            .build(*engine, e);
        auto& transformManager = engine->getTransformManager();
        const auto instance = transformManager.getInstance(e);
        transformManager.setTransform(
            instance,
            composeVolumeTransform(obb.getCenter(), obb.getHalfAxes(),
                                   prepareResources->modelMatrix(),
                                   prepareResources->localOrigin()));
        scene->addEntity(e);
        return e;
    }

    // P36: tile bounding volume (tileset.json) as a yellow line box.
    // Any volume type works: getOrientedBoundingBoxFromBoundingVolume
    // converts sphere/region/OBB to an OBB (regions via the WGS84
    // ellipsoid — exact for our box fixtures, conservative otherwise).
    void updateTileBoundingVolume(
        Cesium3DTilesSelection::Tile& tile, TileRenderData& data) {
        const bool want = debugShowBoundingVolume && data.inScene;
        if (want && !data.bvInScene) {
            const CesiumGeometry::OrientedBoundingBox obb =
                Cesium3DTilesSelection::getOrientedBoundingBoxFromBoundingVolume(
                    tile.getBoundingVolume());
            data.bvEntity = createVolumeEntity(obb, debugLineMaterialBv);
            data.bvInScene = true;
        } else if (!want && data.bvInScene) {
            scene->removeEntities(&data.bvEntity, 1);
            engine->destroy(data.bvEntity);
            data.bvInScene = false;
        }
    }

    // P36: viewer request volume (when the tileset declares one) as a cyan
    // line box. Tiles without a request volume never get an entity.
    void updateTileRequestVolume(
        Cesium3DTilesSelection::Tile& tile, TileRenderData& data) {
        const std::optional<Cesium3DTilesSelection::BoundingVolume>& rq =
            tile.getViewerRequestVolume();
        const bool want =
            debugShowViewerRequestVolume && data.inScene && rq.has_value();
        if (want && !data.rqInScene) {
            const CesiumGeometry::OrientedBoundingBox obb =
                Cesium3DTilesSelection::getOrientedBoundingBoxFromBoundingVolume(
                    rq.value());
            data.rqEntity = createVolumeEntity(obb, debugLineMaterialRq);
            data.rqInScene = true;
        } else if (!want && data.rqInScene) {
            scene->removeEntities(&data.rqEntity, 1);
            engine->destroy(data.rqEntity);
            data.rqInScene = false;
        }
    }

    // P35: add/remove one tile's debug wireframe (FilamentAsset::getWireframe,
    // a LINES renderable of the transformed bounding-box hierarchy) to match
    // the tile's scene membership. P36: re-gated on
    // debugShowContentBoundingVolume (the Inspector's Content Volumes
    // checkbox); P35 wired it to debugShowBoundingVolume.
    // T1 (tilesetio): no gltfio FilamentAsset, so no wireframe. No-op.
    // Render thread only.
    void updateTileWireframe(
        Cesium3DTilesSelection::Tile& tile,
        TileRenderData& data) {
        (void)tile;
        (void)data;
        // No-op for tilesetio backend.
    }

    // P36: re-apply all debug-volume flags to currently-loaded tiles
    // (called when any flag toggles). Render thread only.
    void updateDebugVolumeVisibility() {
        if (tileset == nullptr) {
            return;
        }
        const Cesium3DTilesSelection::Tile* pRoot =
            tileset->getRootTile();
        if (pRoot == nullptr) {
            return;
        }
        std::vector<Cesium3DTilesSelection::Tile*> stack{
            const_cast<Cesium3DTilesSelection::Tile*>(pRoot)};
        while (!stack.empty()) {
            auto* pTile = stack.back();
            stack.pop_back();
            auto* pContent = pTile->getContent().getRenderContent();
            if (pContent != nullptr) {
                auto* pData = static_cast<TileRenderData*>(
                    pContent->getRenderResources());
                if (pData != nullptr) {
                    // T1 (tilesetio): no gltfio wireframe; skip. The tile
                    // bounding volume debug (P36) still works via entities.
                    updateTileBoundingVolume(*pTile, *pData);
                    updateTileRequestVolume(*pTile, *pData);
                }
            }
            for (auto& child : pTile->getChildren()) {
                stack.push_back(&child);
            }
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
    // P17: last traversal's diagnostics, captured in updateTiles().
    int lastSelected = -1;
    std::int32_t lastWorkerQueue = -1;
    std::int32_t lastMainQueue = -1;
    // P20: ID strings of tilesToRenderThisFrame from the last traversal
    // (via TileIdUtilities::createTileIdString), for frustum/LOD tests.
    std::vector<std::string> lastSelectedIds;
    // P12: why the last loadTileset() failed (empty when it succeeded).
    std::string lastError;
    // P32: event callbacks (see Renderer::TilesetEventCallbacks), set via
    // TilesetRenderer::setEventCallbacks().
    Renderer::TilesetEventCallbacks eventCallbacks;
    // P32/P33: last observed TileLoadState per tile, for load/unload/failed
    // transition detection in dispatchFrameEvents(). Keyed by tile pointer
    // (not ID string — the traversal root and implicit tiles can share an
    // empty ID); the ID string is materialized once per tile for event
    // payloads. Reset on loadTileset().
    struct TileStateRecord {
        std::string id;
        Cesium3DTilesSelection::TileLoadState state;
    };
    std::unordered_map<const Cesium3DTilesSelection::Tile*, TileStateRecord>
        tileStates;
    // P32: initialTilesLoaded fires once per loadTileset().
    bool initialTilesLoadedFired = false;
    // P32: last loadProgress payload, for change detection (-1 = never).
    std::int64_t lastProgressPending = -1;
    std::int64_t lastProgressProcessing = -1;
    // P33: last frame's fully-loaded verdict (P32's allTilesLoaded
    // condition), updated by every dispatchFrameEvents() walk so
    // tilesLoaded() is queryable without event callbacks.
    bool lastTilesLoaded = false;
    // P33: show / preloadWhenHidden / modelMatrix (see renderer.h).
    bool show = true;
    bool preloadWhenHidden = false;
    glm::dmat4 modelMatrix{1.0};
    // P35: debug switches (see renderer.h).
    bool debugShowBoundingVolume = false;
    bool debugShowUrl = false;
    // P36: Inspector Display section (see renderer.h / ADR-0035).
    // debugShowBoundingVolume now draws the *tile* bounding volume
    // (tileset.json); the P35 asset wireframe moved to
    // debugShowContentBoundingVolume.
    bool debugShowContentBoundingVolume = false;
    bool debugShowViewerRequestVolume = false;
    // P36: Inspector Update section — skip tile selection/LOD update.
    bool debugFreezeFrame = false;
    // P36: shared debug line-box resources (lazy; see
    // ensureDebugLineResources). The unit-box geometry is shared; each
    // tile's bvEntity/rqEntity carries the volume in its transform.
    filament::Material* debugLineMaterial = nullptr;
    filament::MaterialInstance* debugLineMaterialBv = nullptr; // yellow
    filament::MaterialInstance* debugLineMaterialRq = nullptr; // cyan
    filament::VertexBuffer* debugLineVb = nullptr;
    filament::IndexBuffer* debugLineIb = nullptr;
    // P33: load/update timestamps for timeSinceLoadMs().
    std::chrono::steady_clock::time_point loadTime{};
    std::chrono::steady_clock::time_point firstUpdateTime{};
    bool hasFirstUpdate = false;
    // P34: trimLoadedTiles() sets this; the next updateTiles() briefly
    // zeroes maximumCachedBytes around loadTiles() so cesium-native's
    // unloadCachedBytes evicts everything not in use, then restores it.
    bool trimRequested = false;
    // P34: tileset.json "extensionsUsed", cached at loadTileset time for
    // hasExtension(). Empty when no tileset is loaded.
    std::vector<std::string> extensionsUsed;
    // P32: tileset.json-level failures arrive via
    // TilesetOptions::loadErrorCallback (contract allows worker threads).
    // The callback is owned by the Tileset, so during teardown it could
    // outlive the Impl's other members — therefore it only touches this
    // shared queue, never the Impl itself. Drained on the render thread in
    // dispatchFrameEvents().
    struct LoadFailureQueue {
        std::mutex mutex;
        std::vector<Renderer::TileFailedInfo> pending;
    };
    std::shared_ptr<LoadFailureQueue> loadFailureQueue;
    // P5 rebase origin (world coordinates, double). Tile selection
    // (ViewState) orbits this point in full double precision; rendering
    // subtracts it in double precision before the float32 conversion.
    glm::dvec3 localOrigin{0.0, 0.0, 0.0};
    bool hasOrigin = false;
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

// P31: load with explicit options.
bool TilesetRenderer::load(const std::string& urlOrPath,
                           const Renderer::TilesetOptions& options) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->loadTileset(urlOrPath, options);
#else
    (void)urlOrPath;
    (void)options;
    std::cerr << "[tiles_renderer] loadTileset: not available (built without "
                 "cesium-native + Filament)"
              << std::endl;
    return false;
#endif
}

std::string TilesetRenderer::lastError() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->lastError;
#else
    return "loadTileset: not available (built without cesium-native + Filament)";
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

Renderer::TileStats TilesetRenderer::tileStats() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    Renderer::TileStats stats;
    if (!_impl->loaded || _impl->tileset == nullptr) {
        return stats; // all -1
    }
    stats.selectedTiles = _impl->lastSelected;
    stats.tilesLoading = static_cast<std::int64_t>(_impl->lastWorkerQueue) +
                         static_cast<std::int64_t>(_impl->lastMainQueue);
    // P18: the traversal queues drain into cesium-native's async request
    // scheduler, so queue lengths alone hit 0 while curl downloads are
    // still in flight (observed: 3 tiles downloading, queues at 0). Count
    // tiles in ContentLoading/ContentLoaded too, so "loading" stays honest
    // during slow loads. (A tile is never in both sets at once: queued
    // tiles have not started loading yet.)
    std::int64_t inFlightContent = 0;
    // Walk the instantiated tree once: count finished (Done) tiles and
    // failed tiles. Done-state counting is exact; forEachLoadedTile would
    // also count tiles merely referenced but not yet loaded, which would
    // make "loaded" lie during streaming.
    std::int64_t loadedCount = 0;
    std::int64_t failedCount = 0;
    // P36: tiles visited by the walk (instantiated tree size) for the
    // Inspector statistics section.
    std::int64_t visitedCount = 0;
    const Cesium3DTilesSelection::Tile* pRoot =
        _impl->tileset->getRootTile();
    if (pRoot != nullptr) {
        // Iterative stack — no recursion depth risk on deep trees.
        std::vector<const Cesium3DTilesSelection::Tile*> stack{pRoot};
        while (!stack.empty()) {
            const Cesium3DTilesSelection::Tile* pTile = stack.back();
            stack.pop_back();
            ++visitedCount;
            const auto state = pTile->getState();
            if (state == Cesium3DTilesSelection::TileLoadState::Done) {
                ++loadedCount;
            }
            if (state ==
                    Cesium3DTilesSelection::TileLoadState::ContentLoading ||
                state ==
                    Cesium3DTilesSelection::TileLoadState::ContentLoaded) {
                ++inFlightContent;
            }
            if (state == Cesium3DTilesSelection::TileLoadState::Failed ||
                state ==
                    Cesium3DTilesSelection::TileLoadState::FailedTemporarily) {
                // Permanent and transient failures both count: a transient
                // failure is retried by cesium-native and may clear.
                ++failedCount;
            }
            for (const Cesium3DTilesSelection::Tile& child :
                 pTile->getChildren()) {
                stack.push_back(&child);
            }
        }
    }
    stats.tilesLoaded = loadedCount;
    stats.tilesFailed = failedCount;
    stats.tilesLoading += inFlightContent;
    stats.bytesLoaded = _impl->tileset->getTotalDataBytes();
    // P36: Inspector statistics section.
    stats.tilesVisited = visitedCount;
    stats.pendingRequests = static_cast<std::int64_t>(_impl->lastWorkerQueue) +
                            static_cast<std::int64_t>(_impl->lastMainQueue);
    stats.tilesProcessing = inFlightContent;
    return stats;
#else
    return Renderer::TileStats{};
#endif
}

std::vector<std::string> TilesetRenderer::selectedTileIds() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    if (!_impl->loaded || _impl->tileset == nullptr) {
        return {};
    }
    return _impl->lastSelectedIds;
#else
    return {};
#endif
}

void TilesetRenderer::setMaxCachedBytes(std::int64_t bytes) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    _impl->maxCachedBytes = bytes;
    if (_impl->loaded && _impl->tileset != nullptr) {
        // Tileset::getOptions() has a non-const overload; the content
        // manager reads _options.maximumCachedBytes on every update, so
        // this takes effect on the next frame without reloading.
        // Values <= 0 restore the cesium-native default of 512MB.
        _impl->tileset->getOptions().maximumCachedBytes =
            bytes > 0 ? bytes : (512LL * 1024 * 1024);
    }
#else
    (void)bytes;
#endif
}

// P31: live SSE budget. Same stash-then-forward pattern as
// setMaxCachedBytes: applies to the next load() via pendingMaxSse and to a
// loaded tileset via Tileset::getOptions() (read every updateViewGroup, so
// it takes effect on the next frame). Values < 0, NaN, or +inf restore the
// default 16 — normalized at stash time so pre-load and live agree.
void TilesetRenderer::setMaximumScreenSpaceError(double sse) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    const double normalized =
        (sse >= 0.0 && std::isfinite(sse)) ? sse : 16.0;
    _impl->hasPendingMaxSse = true;
    _impl->pendingMaxSse = normalized;
    if (_impl->loaded && _impl->tileset != nullptr) {
        _impl->tileset->getOptions().maximumScreenSpaceError = normalized;
        _impl->appliedOptions.maximumScreenSpaceError = normalized;
    }
#else
    (void)sse;
#endif
}

double TilesetRenderer::maximumScreenSpaceError() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    if (_impl->loaded && _impl->tileset != nullptr) {
        return _impl->tileset->getOptions().maximumScreenSpaceError;
    }
    if (_impl->hasPendingMaxSse) {
        return _impl->pendingMaxSse;
    }
#endif
    return 16.0;
}

Renderer::TilesetOptions TilesetRenderer::currentOptions() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->appliedOptions;
#else
    return Renderer::TilesetOptions{};
#endif
}

// P32: store event callbacks on the live TilesetRenderer.
void TilesetRenderer::setEventCallbacks(
    const Renderer::TilesetEventCallbacks& callbacks) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    _impl->eventCallbacks = callbacks;
#else
    (void)callbacks;
#endif
}

// P32: fire onTileUnload for every tile whose content is currently loaded.
// Runs on the render thread, before the Tileset object is destroyed.
void TilesetRenderer::fireTileUnloadEvents() {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    if (!_impl->eventCallbacks.onTileUnload) {
        return;
    }
    if (!_impl->loaded || _impl->tileset == nullptr) {
        return;
    }
    const Cesium3DTilesSelection::Tile* pRoot =
        _impl->tileset->getRootTile();
    if (pRoot == nullptr) {
        return;
    }
    std::vector<const Cesium3DTilesSelection::Tile*> stack{pRoot};
    while (!stack.empty()) {
        const auto* pTile = stack.back();
        stack.pop_back();
        const auto it = _impl->tileStates.find(pTile);
        if (it != _impl->tileStates.end() &&
            it->second.state == Cesium3DTilesSelection::TileLoadState::Done) {
            _impl->eventCallbacks.onTileUnload(
                TilesetRenderer::Impl::tileEventInfo(*pTile));
        }
        for (const auto& child : pTile->getChildren()) {
            stack.push_back(&child);
        }
    }
#endif
}

// P33: show / preloadWhenHidden / modelMatrix + read-only state.
void TilesetRenderer::setShow(bool show) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    _impl->show = show;
#else
    (void)show;
#endif
}

bool TilesetRenderer::isShow() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->show;
#else
    return true;
#endif
}

void TilesetRenderer::setPreloadWhenHidden(bool preload) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    _impl->preloadWhenHidden = preload;
#else
    (void)preload;
#endif
}

bool TilesetRenderer::isPreloadWhenHidden() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->preloadWhenHidden;
#else
    return false;
#endif
}

void TilesetRenderer::setModelMatrix(const double matrix[16]) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    glm::dmat4 m;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            m[c][r] = matrix[c * 4 + r];
        }
    }
    _impl->modelMatrix = m;
    if (_impl->prepareResources != nullptr) {
        _impl->prepareResources->setModelMatrix(m);
    }
    // Re-apply to already-loaded tiles immediately: recompose each tile's
    // Filament transform from its stored double-precision pieces. Render
    // thread only (same thread as prepareInMainThread).
    if (_impl->tileset != nullptr && _impl->engine != nullptr) {
        const Cesium3DTilesSelection::Tile* pRoot =
            _impl->tileset->getRootTile();
        if (pRoot != nullptr) {
            auto& transformManager =
                _impl->engine->getTransformManager();
            std::vector<Cesium3DTilesSelection::Tile*> stack{
                const_cast<Cesium3DTilesSelection::Tile*>(pRoot)};
            while (!stack.empty()) {
                auto* pTile = stack.back();
                stack.pop_back();
                auto* pContent = pTile->getContent().getRenderContent();
                if (pContent != nullptr) {
                    auto* pData = static_cast<TileRenderData*>(
                        pContent->getRenderResources());
                    if (pData != nullptr) {
                        // T1 (tilesetio): setModelMatrix recomposition for
                        // tile content not yet implemented (transforms are
                        // baked per-primitive at creation). TODO(T2).
                        // P36: the debug volume line-boxes carry the volume
                        // in their entity transform; recompose them for the
                        // new model matrix too (backend-independent).
                        if (pData->bvInScene) {
                            const auto bvObb = Cesium3DTilesSelection::
                                getOrientedBoundingBoxFromBoundingVolume(
                                    pTile->getBoundingVolume());
                            const auto bvInstance =
                                transformManager.getInstance(
                                    pData->bvEntity);
                            if (bvInstance.isValid()) {
                                transformManager.setTransform(
                                    bvInstance,
                                    Impl::composeVolumeTransform(
                                        bvObb.getCenter(),
                                        bvObb.getHalfAxes(), m,
                                        _impl->localOrigin));
                            }
                        }
                        if (pData->rqInScene) {
                            const auto& rq =
                                pTile->getViewerRequestVolume();
                            if (rq.has_value()) {
                                const auto rqObb = Cesium3DTilesSelection::
                                    getOrientedBoundingBoxFromBoundingVolume(
                                        rq.value());
                                const auto rqInstance =
                                    transformManager.getInstance(
                                        pData->rqEntity);
                                if (rqInstance.isValid()) {
                                    transformManager.setTransform(
                                        rqInstance,
                                        Impl::composeVolumeTransform(
                                            rqObb.getCenter(),
                                            rqObb.getHalfAxes(), m,
                                            _impl->localOrigin));
                                }
                            }
                        }
                    }
                }
                for (auto& child : pTile->getChildren()) {
                    stack.push_back(&child);
                }
            }
        }
    }
#else
    (void)matrix;
#endif
}

void TilesetRenderer::modelMatrix(double out[16]) const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    const glm::dmat4& m = _impl->modelMatrix;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c * 4 + r] = m[c][r];
        }
    }
#else
    for (int i = 0; i < 16; ++i) {
        out[i] = (i % 5 == 0) ? 1.0 : 0.0;
    }
#endif
}

bool TilesetRenderer::tilesLoaded() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->loaded && _impl->lastTilesLoaded;
#else
    return false;
#endif
}

Renderer::BoundingSphere TilesetRenderer::boundingSphere() const {
    Renderer::BoundingSphere out{{0.0, 0.0, 0.0}, 0.0};
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    if (!_impl->loaded || _impl->tileset == nullptr) {
        return out;
    }
    const Cesium3DTilesSelection::Tile* pRoot =
        _impl->tileset->getRootTile();
    if (pRoot == nullptr) {
        return out;
    }
    // Root bounding volume -> sphere (world space, authored transform).
    glm::dvec3 center(0.0);
    double radius = 0.0;
    bool ok = false;
    const Cesium3DTilesSelection::BoundingVolume& bv =
        pRoot->getBoundingVolume();
    if (const auto* pBox = std::get_if<CesiumGeometry::OrientedBoundingBox>(
            &bv);
        pBox != nullptr) {
        center = pBox->getCenter();
        const glm::dmat3& h = pBox->getHalfAxes();
        radius = std::sqrt(
            glm::dot(h[0], h[0]) + glm::dot(h[1], h[1]) +
            glm::dot(h[2], h[2]));
        ok = true;
    } else if (const auto* pSphere =
                   std::get_if<CesiumGeometry::BoundingSphere>(&bv);
               pSphere != nullptr) {
        center = pSphere->getCenter();
        radius = pSphere->getRadius();
        ok = true;
    } else {
        // Region / region-with-loose-heights -> their bounding box.
        const CesiumGeometry::OrientedBoundingBox* pB = nullptr;
        std::optional<CesiumGeometry::OrientedBoundingBox> box;
        if (const auto* pRegion =
                std::get_if<CesiumGeospatial::BoundingRegion>(&bv);
            pRegion != nullptr) {
            box = pRegion->getBoundingBox();
        } else if (const auto* pLoose = std::get_if<
                       CesiumGeospatial::BoundingRegionWithLooseFittingHeights>(
                       &bv);
                   pLoose != nullptr) {
            box = pLoose->getBoundingRegion().getBoundingBox();
        }
        if (box.has_value()) {
            pB = &box.value();
        }
        if (pB != nullptr) {
            center = pB->getCenter();
            const glm::dmat3& h = pB->getHalfAxes();
            radius = std::sqrt(
                glm::dot(h[0], h[0]) + glm::dot(h[1], h[1]) +
                glm::dot(h[2], h[2]));
            ok = true;
        }
    }
    if (!ok) {
        return out;
    }
    // P33: apply modelMatrix (cesium.js applies it to boundingSphere).
    // Radius scales by the matrix's maximum axis scale (conservative for
    // non-uniform scale).
    const glm::dmat4& m = _impl->modelMatrix;
    const glm::dvec3 wc = glm::dvec3(m * glm::dvec4(center, 1.0));
    const double s = std::max(
        {glm::length(m[0]), glm::length(m[1]), glm::length(m[2])});
    out.center[0] = wc.x;
    out.center[1] = wc.y;
    out.center[2] = wc.z;
    out.radius = radius * s;
    return out;
#else
    return out;
#endif
}

std::int64_t TilesetRenderer::timeSinceLoadMs() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    if (!_impl->loaded || !_impl->hasFirstUpdate) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - _impl->firstUpdateTime)
        .count();
#else
    return 0;
#endif
}

std::string TilesetRenderer::rootTileId() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    if (!_impl->loaded || _impl->tileset == nullptr) {
        return "";
    }
    const Cesium3DTilesSelection::Tile* pRoot =
        _impl->tileset->getRootTile();
    if (pRoot == nullptr) {
        return "";
    }
    // Unwrap cesium-native's internal empty-ID wrapper tile: the real
    // tileset.json root is its single child.
    const Cesium3DTilesSelection::Tile* p = pRoot;
    while (p != nullptr &&
           Cesium3DTilesSelection::TileIdUtilities::createTileIdString(
               p->getTileID())
                   .empty() &&
           p->getChildren().size() == 1) {
        p = &p->getChildren()[0];
    }
    if (p == nullptr) {
        return "";
    }
    return Cesium3DTilesSelection::TileIdUtilities::createTileIdString(
        p->getTileID());
#else
    return "";
#endif
}

// P34: content bytes currently held (cesium-native getTotalDataBytes).
// Same value as TileStats::bytesLoaded. 0 when no tileset is loaded.
std::int64_t TilesetRenderer::totalMemoryUsageInBytes() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    if (!_impl->loaded || _impl->tileset == nullptr) {
        return 0;
    }
    return _impl->tileset->getTotalDataBytes();
#else
    return 0;
#endif
}

// P34: request a cache trim on the next updateTiles(). The flag is
// consumed there (briefly zeroes maximumCachedBytes around loadTiles()).
void TilesetRenderer::trimLoadedTiles() {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    if (_impl->loaded && _impl->tileset != nullptr) {
        _impl->trimRequested = true;
    }
#endif
}

// P34: whether tileset.json declared `name` in top-level extensionsUsed
// (cached at loadTileset time).
bool TilesetRenderer::hasExtension(const std::string& name) const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    if (!_impl->loaded) {
        return false;
    }
    return std::find(
               _impl->extensionsUsed.begin(),
               _impl->extensionsUsed.end(),
               name) != _impl->extensionsUsed.end();
#else
    (void)name;
    return false;
#endif
}

// P35: debug switches. P36: the volume flags re-apply to all
// currently-loaded tiles immediately (same pattern as P33 setModelMatrix
// re-applying transforms).
void TilesetRenderer::setDebugShowBoundingVolume(bool show) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    _impl->debugShowBoundingVolume = show;
    _impl->updateDebugVolumeVisibility();
#else
    (void)show;
#endif
}

bool TilesetRenderer::isDebugShowBoundingVolume() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->debugShowBoundingVolume;
#else
    return false;
#endif
}

void TilesetRenderer::setDebugShowContentBoundingVolume(bool show) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    _impl->debugShowContentBoundingVolume = show;
    _impl->updateDebugVolumeVisibility();
#else
    (void)show;
#endif
}

bool TilesetRenderer::isDebugShowContentBoundingVolume() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->debugShowContentBoundingVolume;
#else
    return false;
#endif
}

void TilesetRenderer::setDebugShowViewerRequestVolume(bool show) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    _impl->debugShowViewerRequestVolume = show;
    _impl->updateDebugVolumeVisibility();
#else
    (void)show;
#endif
}

bool TilesetRenderer::isDebugShowViewerRequestVolume() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->debugShowViewerRequestVolume;
#else
    return false;
#endif
}

void TilesetRenderer::setDebugFreezeFrame(bool freeze) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    _impl->debugFreezeFrame = freeze;
#else
    (void)freeze;
#endif
}

bool TilesetRenderer::isDebugFreezeFrame() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->debugFreezeFrame;
#else
    return false;
#endif
}

void TilesetRenderer::setDebugShowUrl(bool show) {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    _impl->debugShowUrl = show;
#else
    (void)show;
#endif
}

bool TilesetRenderer::isDebugShowUrl() const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    return _impl->debugShowUrl;
#else
    return false;
#endif
}

// P37-C1: expose the tileset's local origin (world coords, double) so the
// explicit camera (world space) can be rebased the same way as tiles.
void TilesetRenderer::localOrigin(double out[3]) const {
#if defined(TILES_WITH_CESIUM_NATIVE) && defined(TILES_WITH_FILAMENT)
    const glm::dvec3& o = _impl->localOrigin;
    out[0] = o.x;
    out[1] = o.y;
    out[2] = o.z;
#else
    out[0] = out[1] = out[2] = 0.0;
#endif
}

} // namespace tiles_renderer
