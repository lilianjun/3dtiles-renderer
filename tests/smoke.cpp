// P0 smoke test: verifies the toolchain, the generated version header,
// and that a test binary can build + run under CTest.

#include <cassert>
#include <iostream>

#include "tiles_renderer/version.h"

int main() {
    static_assert(__cplusplus >= 202002L, "C++20 required");

    std::cout << "[smoke] version=" << TILES_RENDERER_VERSION
              << " platform=" << TILES_RENDERER_PLATFORM << std::endl;

    assert(TILES_RENDERER_VERSION[0] != '\0');
    assert(TILES_RENDERER_PLATFORM[0] != '\0');

    std::cout << "[smoke] OK" << std::endl;
    return 0;
}
