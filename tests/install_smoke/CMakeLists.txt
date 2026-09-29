# P14: minimal external consumer for the installed SDK.
# CI-only fixture: deliberately NOT registered in tests/CMakeLists.txt, so
# the ordinary ctest suite is unaffected. CI installs the SDK to a temp
# prefix, then configures this project with
#   -DCMAKE_PREFIX_PATH=<install prefix>
cmake_minimum_required(VERSION 3.28)
project(tiles_install_smoke CXX)

find_package(tiles_renderer CONFIG REQUIRED)

add_executable(install_smoke main.cpp)
target_link_libraries(install_smoke PRIVATE tiles_renderer::tiles_renderer)
