// text_renderer_stb.cpp — stb_truetype text rendering implementation.

#include "text_renderer_stb.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <cstring>
#include <algorithm>

namespace tiles_renderer {

struct TextRendererStb::Impl {
    stbtt_fontinfo font;
    std::vector<uint8_t> fontData;
    bool fontLoaded = false;
};

TextRendererStb::TextRendererStb() : _impl(std::make_unique<Impl>()) {}
TextRendererStb::~TextRendererStb() = default;

bool TextRendererStb::loadFont(const std::string& fontPath) {
    FILE* f = fopen(fontPath.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    _impl->fontData.resize(size);
    fread(_impl->fontData.data(), 1, size, f);
    fclose(f);
    return loadFontFromMemory(_impl->fontData.data(), size);
}

bool TextRendererStb::loadFontFromMemory(const uint8_t* data, size_t size) {
    // Copy the data (stbtt needs it to persist)
    _impl->fontData.assign(data, data + size);
    if (!stbtt_InitFont(&_impl->font, _impl->fontData.data(), 0)) {
        return false;
    }
    _impl->fontLoaded = true;
    return true;
}

bool TextRendererStb::hasFont() const {
    return _impl->fontLoaded;
}

void TextRendererStb::setLabels(const std::vector<TextLabelStb>& labels) {
    _labels = labels;
}

void TextRendererStb::clear() {
    _labels.clear();
}

bool TextRendererStb::renderToBitmap(
    std::vector<uint8_t>& outPixels, int& outW, int& outH) {
    if (_labels.empty() || !_impl->fontLoaded) {
        return false;
    }

    // Calculate bitmap size (simple: fixed size, render all labels)
    outW = 1024;
    outH = 1024;
    outPixels.assign(outW * outH * 4, 0);

    for (const auto& label : _labels) {
        float scale = stbtt_ScaleForPixelHeight(&_impl->font, label.fontSize);
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&_impl->font, &ascent, &descent, &lineGap);
        int baseline = static_cast<int>(ascent * scale);

        float x = label.x;
        float y = label.y + baseline;

        for (char c : label.text) {
            if (c < 32 || c > 126) continue;

            int advance, lsb;
            stbtt_GetCodepointHMetrics(&_impl->font, c, &advance, &lsb);

            int glyphW, glyphH, xoff, yoff;
            uint8_t* bitmap = stbtt_GetCodepointBitmap(
                &_impl->font, scale, scale, c, &glyphW, &glyphH, &xoff, &yoff);

            // Blit glyph to output
            for (int gy = 0; gy < glyphH; ++gy) {
                for (int gx = 0; gx < glyphW; ++gx) {
                    int px = static_cast<int>(x) + xoff + gx;
                    int py = static_cast<int>(y) + yoff + gy;
                    if (px >= 0 && px < outW && py >= 0 && py < outH) {
                        uint8_t alpha = bitmap[gy * glyphW + gx];
                        int idx = (py * outW + px) * 4;
                        // Alpha blend
                        float a = alpha / 255.0f * label.color[3];
                        outPixels[idx + 0] = static_cast<uint8_t>(
                            label.color[0] * 255 * a + outPixels[idx + 0] * (1 - a));
                        outPixels[idx + 1] = static_cast<uint8_t>(
                            label.color[1] * 255 * a + outPixels[idx + 1] * (1 - a));
                        outPixels[idx + 2] = static_cast<uint8_t>(
                            label.color[2] * 255 * a + outPixels[idx + 2] * (1 - a));
                        outPixels[idx + 3] = 255;
                    }
                }
            }
            stbtt_FreeBitmap(bitmap, nullptr);

            x += advance * scale;
        }
    }

    return true;
}

} // namespace tiles_renderer
