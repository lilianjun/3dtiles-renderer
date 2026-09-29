// tiles_renderer SDK — 3D Tiles tileset integration (P3).
//
// Loads a tileset.json via cesium-native, selects tiles per frame with
// updateViewGroup, and converts each tile's glb content into Filament
// renderables with gltfio (+ ubershader materials).

#include "tileset_internal.h"

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
// P22-hotfix: glm and curl are only available when cesium-native is built
// (glm arrives via cesium-native's vcpkg tree; curl via its vcpkg ports).
// The Windows/Android/iOS/WASM CI configs build the SDK WITHOUT
// cesium-native, so these includes must stay inside this guard — an
// unconditional include here broke all four platforms (C1083 /
// 'glm/gtc/matrix_transform.hpp' file not found, P22 CI).
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <curl/curl.h>
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
#ifdef TILES_WITH_STB_PROVIDER
#include <gltfio/TextureProvider.h> // createStbProvider (P15: glTF textures)
#endif
#include <gltfio/materials/uberarchive.h>
#include <utils/Entity.h>
#endif

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

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
// The glb bytes are kept alive for the asset's lifetime (gltfio references
// the caller's buffer).
// ---------------------------------------------------------------------------
struct TileRenderData {
    std::vector<std::uint8_t> glbBytes;
    filament::gltfio::FilamentAsset* asset = nullptr;
    bool inScene = false;
};

// ---------------------------------------------------------------------------
// Load-thread result handed from prepareInLoadThread to prepareInMainThread.
// glbBytes: the tile content as binary glb (copied raw, or re-serialized
// from a converted CesiumGltf::Model). rtcCenter: b3dm RTC_CENTER in tile-
// local coordinates (double); applied to the transform in double precision
// in prepareInMainThread instead of being baked into float glTF nodes.
// upAxisFix: for i3dm-converted models, the glTF up-axis-to-Z-up matrix the
// converter assumed the runtime would apply (see modelToGlb); identity for
// everything else.
// ---------------------------------------------------------------------------
struct LoadThreadData {
    std::vector<std::uint8_t> glbBytes;
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
// P7: EXT_mesh_gpu_instancing nodes are expanded into plain nodes first
// (see expandGpuInstancing), and multi-buffer models are merged into the
// single GLB BIN chunk (the i3dm converter appends an instance-data
// buffer; writing only buffers[0] would dangle the instance accessors).
LoadThreadData* modelToGlb(const CesiumGltf::Model& inModel) {
    CesiumGltf::Model model = inModel; // copy: we strip the RTC extension
    glm::dvec3 rtcCenter{0.0, 0.0, 0.0};
    if (const auto* pRtc =
            model.getExtension<CesiumGltf::ExtensionCesiumRTC>();
        pRtc != nullptr && pRtc->center.size() == 3) {
        rtcCenter = glm::dvec3(
            pRtc->center[0], pRtc->center[1], pRtc->center[2]);
        model.extensions.erase(CesiumGltf::ExtensionCesiumRTC::ExtensionName);
        model.removeExtensionUsed(CesiumGltf::ExtensionCesiumRTC::ExtensionName);
        model.removeExtensionRequired(
            CesiumGltf::ExtensionCesiumRTC::ExtensionName);
    }
    // P7: detect i3dm-converted models BEFORE expandGpuInstancing strips
    // the extension. Only the i3dm converter adds EXT_mesh_gpu_instancing,
    // and only it conjugates instance transforms by upToZ (see above), so
    // only those models need the compensating rotation at the asset root.
    bool fromI3dm = false;
    for (const auto& node : model.nodes) {
        if (node.getExtension<CesiumGltf::ExtensionExtMeshGpuInstancing>() !=
            nullptr) {
            fromI3dm = true;
            break;
        }
    }
    const glm::dmat4 upAxisFix = fromI3dm ? upAxisToZUp(model)
                                          : glm::dmat4(1.0);
    expandGpuInstancing(model);
    std::span<const std::byte> bufferData;
    std::vector<std::byte> mergedBuffers;
    if (!model.buffers.empty()) {
        // P7: a GLB has a single BIN chunk, but converted models may carry
        // several buffers — the i3dm converter appends an instance-data
        // buffer holding the EXT_mesh_gpu_instancing TRANSLATION/ROTATION/
        // SCALE accessors. Merge every buffer's data into one chunk and
        // repoint all bufferViews at buffer 0 with adjusted byteOffsets;
        // writing only buffers[0] would dangle the instance accessors.
        std::vector<std::size_t> base(model.buffers.size(), 0);
        for (std::size_t i = 0; i < model.buffers.size(); ++i) {
            base[i] = mergedBuffers.size();
            const auto& bytes = model.buffers[i].cesium.data;
            mergedBuffers.insert(mergedBuffers.end(), bytes.begin(),
                                 bytes.end());
            while (mergedBuffers.size() % 4 != 0) {
                mergedBuffers.push_back(std::byte{0});
            }
        }
        for (auto& bufferView : model.bufferViews) {
            if (bufferView.buffer >= 0 &&
                static_cast<std::size_t>(bufferView.buffer) < base.size()) {
                bufferView.byteOffset +=
                    static_cast<std::int64_t>(base[bufferView.buffer]);
                bufferView.buffer = 0;
            }
        }
        model.buffers.resize(1);
        model.buffers[0].cesium.data.assign(mergedBuffers.begin(),
                                            mergedBuffers.end());
        model.buffers[0].byteLength =
            static_cast<std::int64_t>(mergedBuffers.size());
        bufferData = std::span<const std::byte>(
            model.buffers[0].cesium.data.data(),
            model.buffers[0].cesium.data.size());
    }
    CesiumGltfWriter::GltfWriter writer;
    CesiumGltfWriter::GltfWriterResult result = writer.writeGlb(model, bufferData);
    if (!result.errors.empty()) {
        std::cerr << "[tiles_renderer] modelToGlb: writeGlb failed: "
                  << result.errors.front() << std::endl;
        return nullptr;
    }
    auto* pData = new LoadThreadData();
    pData->rtcCenter = rtcCenter;
    pData->upAxisFix = upAxisFix;
    pData->glbBytes.assign(
        reinterpret_cast<const std::uint8_t*>(result.gltfBytes.data()),
        reinterpret_cast<const std::uint8_t*>(result.gltfBytes.data()) +
            result.gltfBytes.size());
    return pData;
}

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
#ifdef TILES_WITH_STB_PROVIDER
        // P15: without a texture provider, gltfio logs "Missing texture
        // provider for image/png" and textured materials render black.
        // The stb decoder ships in the Filament prebuilt package (libstb.a).
        _textureProvider = filament::gltfio::createStbProvider(engine);
        _resourceLoader->addTextureProvider("image/png", _textureProvider);
        _resourceLoader->addTextureProvider("image/jpeg", _textureProvider);
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
    }

    CesiumAsync::Future<
        Cesium3DTilesSelection::TileLoadResultAndRenderResources>
    prepareInLoadThread(
        const CesiumAsync::AsyncSystem& asyncSystem,
        Cesium3DTilesSelection::TileLoadResult&& tileLoadResult,
        const glm::dmat4& /*transform*/,
        const std::any& /*rendererOptions*/) override {
        LoadThreadData* pData = nullptr;
        const auto& result = tileLoadResult;

        // Case A: cesium-native already converted the content to a
        // CesiumGltf::Model (b3dm / i3dm / ... via GltfConverters).
        // Serialize it back to glb bytes for gltfio.
        if (const auto* pModel =
                std::get_if<CesiumGltf::Model>(&result.contentKind);
            pModel != nullptr) {
            pData = modelToGlb(*pModel);
        }

        // Case B: raw glb bytes straight from the completed request.
        if (pData == nullptr && result.pCompletedRequest != nullptr &&
            result.pCompletedRequest->response() != nullptr) {
            const auto data = result.pCompletedRequest->response()->data();
            if (data.size() >= 4) {
                std::uint32_t magic = 0;
                std::memcpy(&magic, data.data(), 4);
                if (magic == kGltfMagic) {
                    pData = new LoadThreadData();
                    pData->glbBytes.assign(
                        reinterpret_cast<const std::uint8_t*>(data.data()),
                        reinterpret_cast<const std::uint8_t*>(data.data()) +
                            data.size());
                }
            }
        }
        Cesium3DTilesSelection::TileLoadResultAndRenderResources out;
        out.result = std::move(tileLoadResult);
        out.pRenderResources = pData;
        return asyncSystem.createResolvedFuture<
            Cesium3DTilesSelection::TileLoadResultAndRenderResources>(
            std::move(out));
    }

    void* prepareInMainThread(
        Cesium3DTilesSelection::Tile& tile, void* pLoadThreadResult) override {
        auto* pLoad =
            static_cast<LoadThreadData*>(pLoadThreadResult);
        if (pLoad == nullptr || pLoad->glbBytes.empty()) {
            delete pLoad;
            return nullptr; // not glb content (or load failed)
        }
        auto* pData = new TileRenderData();
        pData->glbBytes = std::move(pLoad->glbBytes);
        const glm::dvec3 rtcCenter = pLoad->rtcCenter;
        const glm::dmat4 upAxisFix = pLoad->upAxisFix;
        delete pLoad;

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

        // Compose the tile's world transform in double precision:
        //   world = tileTransform * translate(rtcCenter) * upAxisFix
        //           - localOrigin
        // The b3dm RTC_CENTER (if any) is applied here in double precision
        // instead of being baked into the float glTF node, and localOrigin
        // (P5 rebase) keeps huge ECEF-style coordinates renderable in
        // float32. upAxisFix cancels the up-axis conjugation the i3dm
        // converter applies to instance transforms (identity for b3dm /
        // raw glb). Both rtcCenter and upAxisFix are no-ops for the small
        // local test tilesets.
        glm::dmat4 worldT = tile.getTransform();
        if (rtcCenter != glm::dvec3(0.0)) {
            glm::dmat4 rtcT(1.0);
            rtcT[3][0] = rtcCenter.x;
            rtcT[3][1] = rtcCenter.y;
            rtcT[3][2] = rtcCenter.z;
            worldT = worldT * rtcT;
        }
        worldT = worldT * upAxisFix;
        worldT[3] -= glm::dvec4(_localOrigin, 0.0);
        filament::math::mat4f m;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                m[c][r] = static_cast<float>(worldT[c][r]);
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

    // P5 rebase origin (world coordinates, double). Set once per tileset
    // after the root tile loads; defaults to (0,0,0) = no rebase.
    void setLocalOrigin(const glm::dvec3& origin) { _localOrigin = origin; }

    void free(
        Cesium3DTilesSelection::Tile& /*tile*/, void* pLoadThreadResult,
        void* pMainThreadResult) noexcept override {
        // Case 1: prepareInMainThread never ran — drop the load-thread data.
        delete static_cast<LoadThreadData*>(pLoadThreadResult);
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
#ifdef TILES_WITH_STB_PROVIDER
    // P15: stb image decoder feeding gltfio's ResourceLoader (PNG/JPEG).
    // Must outlive _resourceLoader; destroyed after it above.
    filament::gltfio::TextureProvider* _textureProvider = nullptr;
#endif
    // P5 rebase origin (world coordinates, double); subtracted from every
    // tile translation in double precision before the float32 conversion.
    glm::dvec3 _localOrigin{0.0, 0.0, 0.0};
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

    // P19: pending cache budget; applied to TilesetOptions at construction
    // and live-mutated afterwards. -1 = cesium-native default (512MB).
    std::int64_t maxCachedBytes = -1;

    bool loadTileset(const std::string& urlOrPath) {
        lastError.clear();
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
        }
        // Register cesium-native's tile content converters (glTF, b3dm, etc.).
        // Without this, GLB magic bytes are not recognized and tile loads
        // fail. Must be called once before any Tileset is created.
        // P6: std::call_once instead of a bool flag (thread-safe; the old
        // flag raced if two tilesets loaded concurrently on first use).
        static std::once_flag contentTypesRegisteredFlag;
        std::call_once(contentTypesRegisteredFlag, []() {
            Cesium3DTilesContent::registerAllTileContentTypes();
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
        tileset = std::move(newTileset);
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

        // Orbit camera -> cesium ViewState. The orbit target is the tileset's
        // rebase origin (double precision); rendering shows the same tiles
        // rebased around (0,0,0) — see FilamentPrepareResources.
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
                Cesium3DTilesSelection::TileIdUtilities::createTileIdString(
                    pTile->getTileID()));
        }

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
    // P17: last traversal's diagnostics, captured in updateTiles().
    int lastSelected = -1;
    std::int32_t lastWorkerQueue = -1;
    std::int32_t lastMainQueue = -1;
    // P20: ID strings of tilesToRenderThisFrame from the last traversal
    // (via TileIdUtilities::createTileIdString), for frustum/LOD tests.
    std::vector<std::string> lastSelectedIds;
    // P12: why the last loadTileset() failed (empty when it succeeded).
    std::string lastError;
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
    const Cesium3DTilesSelection::Tile* pRoot =
        _impl->tileset->getRootTile();
    if (pRoot != nullptr) {
        // Iterative stack — no recursion depth risk on deep trees.
        std::vector<const Cesium3DTilesSelection::Tile*> stack{pRoot};
        while (!stack.empty()) {
            const Cesium3DTilesSelection::Tile* pTile = stack.back();
            stack.pop_back();
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

} // namespace tiles_renderer
