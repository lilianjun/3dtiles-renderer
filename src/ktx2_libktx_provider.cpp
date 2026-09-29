// P25: KTX2 (KHR_texture_basisu / image/ktx2) texture provider.
//
// Why a custom provider instead of filament's createKtx2Provider?
//
// Filament's Ktx2Provider links Filament's bundled basisu transcoder
// (libbasis_transcoder.a, namespace basist::). On platforms where this SDK
// also builds cesium-native (Linux), cesium's vcpkg libktx.a vendors its OWN
// copy of basisu under the SAME basist:: namespace (different version, different
// class layout). The static linker resolves filament's ktxreader references
// against cesium's incompatible basisu -> heap corruption (proven: standalone
// probe of filament's transcoder works perfectly; linking cesium's libktx.a
// first segfaults on the same file; the demo's ASan trace showed cesium's
// buildtrees/ktx/.../basisu_transcoder.cpp executing inside filament's
// transcodeImageLevel).
//
// So on cesium builds we do NOT link ktxreader/basis_transcoder at all.
// Instead this provider transcodes KTX2 using the very same libktx that
// cesium-native already links (single basist:: in the binary, no conflict),
// decoding to 32-bit RGBA and uploading as a regular RGBA8/SRGB8_A8 texture.
//
// Tradeoff (documented in ADR-0024): we transcode to uncompressed RGBA32
// rather than a GPU block-compressed format. Correctness first; the GPU
// memory saving of BasisU is future work.
//
// On platforms WITHOUT cesium-native (Windows/Android/iOS: no libktx, hence
// no symbol conflict), tileset.cpp uses filament's createKtx2Provider
// directly (TILES_WITH_KTX2_PROVIDER).

#include "ktx2_provider_internal.h"

#ifdef TILES_WITH_KTX2_LIBKTX

#include <ktx.h>

#include <filament/Engine.h>
#include <filament/Texture.h>
#include <gltfio/TextureProvider.h>

#include <utils/BitmaskEnum.h>

#include <deque>
#include <string>
#include <vector>

namespace tiles {

using namespace filament;
using namespace filament::gltfio;

class LibktxKtx2Provider final : public TextureProvider {
public:
    explicit LibktxKtx2Provider(Engine* engine) : mEngine(engine) {}
    ~LibktxKtx2Provider() override = default;

    Texture* pushTexture(const uint8_t* data, size_t byteCount,
            const char* mimeType, TextureFlags flags) override {
        (void)mimeType;
        mPushMessage.clear();

        ktxTexture2* ktx = nullptr;
        ktxResult res = ktxTexture2_CreateFromMemory(
            data, static_cast<ktx_size_t>(byteCount),
            KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx);
        if (res != KTX_SUCCESS || ktx == nullptr) {
            mPushMessage = "ktxTexture2_CreateFromMemory failed";
            return nullptr;
        }

        // Only plain 2D textures; cubemaps / arrays are out of scope.
        if (ktx->numFaces != 1 || ktx->numLayers != 1 || ktx->isCubemap) {
            ktxTexture_Destroy(ktxTexture(ktx));
            mPushMessage = "only 2D KTX2 textures are supported";
            return nullptr;
        }

        // Transcode to 32-bit RGBA. This is the same fallback cesium-native's
        // own ImageDecoder uses (KTX_TTF_RGBA32) and is supported for both
        // ETC1S and UASTC basis formats.
        res = ktxTexture2_TranscodeBasis(ktx, KTX_TTF_RGBA32, 0);
        if (res != KTX_SUCCESS) {
            ktxTexture_Destroy(ktxTexture(ktx));
            mPushMessage = "ktxTexture2_TranscodeBasis(RGBA32) failed";
            return nullptr;
        }

        const uint32_t width = ktx->baseWidth;
        const uint32_t height = ktx->baseHeight;
        if (width == 0 || height == 0 || ktx->pData == nullptr) {
            ktxTexture_Destroy(ktxTexture(ktx));
            mPushMessage = "transcoded KTX2 has no image data";
            return nullptr;
        }
        ktx_size_t imageOffset = 0;
        ktxTexture_GetImageOffset(ktxTexture(ktx), 0, 0, 0, &imageOffset);
        const size_t rgbaBytes = static_cast<size_t>(width) * height * 4;

        // Copy the pixels; the ktx object is destroyed below.
        std::vector<uint8_t>* rgba =
            new std::vector<uint8_t>(ktx->pData + imageOffset,
                                     ktx->pData + imageOffset + rgbaBytes);
        ktxTexture_Destroy(ktxTexture(ktx));

        const bool srgb = any(flags & TextureFlags::sRGB);
        Texture* texture = Texture::Builder()
                .width(width)
                .height(height)
                .levels(0xff)
                .format(srgb ? Texture::InternalFormat::SRGB8_A8
                             : Texture::InternalFormat::RGBA8)
                .usage(Texture::Usage::DEFAULT | Texture::Usage::GEN_MIPMAPPABLE)
                .build(*mEngine);
        if (texture == nullptr) {
            delete rgba;
            mPushMessage = "Unable to build Texture object.";
            return nullptr;
        }

        // Synchronous upload (gltfio calls us on the foreground/engine thread).
        Texture::PixelBufferDescriptor pbd(
            rgba->data(), rgba->size(),
            Texture::Format::RGBA, Texture::Type::UBYTE,
            [](void* /*buf*/, size_t /*size*/, void* user) {
                delete static_cast<std::vector<uint8_t>*>(user);
            },
            rgba);
        texture->setImage(*mEngine, 0, std::move(pbd));
        texture->generateMipmaps(*mEngine);

        ++mPushedCount;
        mReady.push_back(texture);
        return texture;
    }

    Texture* popTexture() override {
        if (mReady.empty()) {
            return nullptr;
        }
        Texture* texture = mReady.front();
        mReady.pop_front();
        ++mPoppedCount;
        return texture;
    }

    void updateQueue() override {
        // Everything is uploaded synchronously in pushTexture; nothing to do.
    }

    void waitForCompletion() override {}
    void cancelDecoding() override {}

    const char* getPushMessage() const override {
        return mPushMessage.empty() ? nullptr : mPushMessage.c_str();
    }
    const char* getPopMessage() const override { return nullptr; }
    size_t getPushedCount() const override { return mPushedCount; }
    size_t getPoppedCount() const override { return mPoppedCount; }
    size_t getDecodedCount() const override { return mPushedCount; }

private:
    Engine* mEngine;
    std::deque<Texture*> mReady; // owned by the ResourceLoader/asset, not us
    std::string mPushMessage;
    size_t mPushedCount = 0;
    size_t mPoppedCount = 0;
};

TextureProvider* createLibktxKtx2Provider(Engine* engine) {
    return new LibktxKtx2Provider(engine);
}

} // namespace tiles

#endif // TILES_WITH_KTX2_LIBKTX
