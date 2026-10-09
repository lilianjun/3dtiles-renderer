// text_renderer_stb.h — Proper text rendering using stb_truetype.
// Replaces the 8x8 bitmap font hack with real TTF font support.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace tiles_renderer {

// A single text label.
struct TextLabelStb {
    std::string text;
    float x, y;  // Screen position in pixels (top-left origin)
    float color[4];
    float fontSize;  // in pixels
};

// TrueType text renderer using stb_truetype.
class TextRendererStb {
public:
    TextRendererStb();
    ~TextRendererStb();

    // Load a TTF font from file. Returns false on failure.
    bool loadFont(const std::string& fontPath);

    // Load a TTF font from memory. Returns false on failure.
    bool loadFontFromMemory(const uint8_t* data, size_t size);

    void setLabels(const std::vector<TextLabelStb>& labels);
    void clear();

    // Render all labels to an RGBA bitmap.
    // Returns false if no labels or no font loaded.
    bool renderToBitmap(std::vector<uint8_t>& outPixels, int& outW, int& outH);

    bool hasFont() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    std::vector<TextLabelStb> _labels;
};

} // namespace tiles_renderer
