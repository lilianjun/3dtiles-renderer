#include <iostream>

#include "tiles_renderer/version.h"

int main() {
    std::cout << "3dtiles-renderer v" << TILES_RENDERER_VERSION
              << " (platform: " << TILES_RENDERER_PLATFORM << ")" << std::endl;
    return 0;
}
