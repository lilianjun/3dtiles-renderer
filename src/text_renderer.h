// text_renderer.h — SDK-level bitmap text rendering for debug labels.
//
// Uses an embedded 8x8 bitmap font (public domain). No external dependencies.
// Renders screen-space text labels for debug visualization
// (debugShowGeometricError, debugShowRenderingStatistics, debugShowMemoryUsage).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tiles_renderer {

// A single text label.
struct TextLabel {
    std::string text;
    float ndcPos[2];  // NDC position (-1..1), origin center
    float color[4];   // RGBA
    float scale;      // Pixel scale multiplier
};

// Bitmap text renderer.
class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    void setLabels(const std::vector<TextLabel>& labels);
    void clear();

    // Get the rendered bitmap (RGBA, width*height*4 bytes).
    // Returns false if no labels.
    bool renderToBitmap(std::vector<uint8_t>& outPixels, int& outW, int& outH);

private:
    std::vector<TextLabel> _labels;
};

} // namespace tiles_renderer
