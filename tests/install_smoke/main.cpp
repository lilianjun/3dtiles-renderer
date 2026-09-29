// P14: CI install smoke test — external consumer of the installed SDK.
// Only calls Renderer::version(): a pure function that needs no display,
// no Filament engine, and no tileset. Rendering is already covered by the
// headless ctest suite; this fixture proves only that the installed SDK
// (headers + static lib + CMake config) is consumable via find_package.
#include <tiles_renderer/renderer.h>

#include <cstdio>
#include <cstring>

int main() {
  const char* v = tiles_renderer::Renderer::version();
  if (v == nullptr || std::strlen(v) == 0) {
    std::fprintf(stderr, "install_smoke: version() returned empty\n");
    return 1;
  }
  std::printf("install_smoke: sdk version: %s\n", v);
  return 0;
}
