# P0 stub test suite. Real unit tests land in P1+ alongside each module.

add_executable(tiles_renderer_smoke smoke.cpp)
target_include_directories(tiles_renderer_smoke PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${CMAKE_BINARY_DIR}/generated"
)
target_compile_features(tiles_renderer_smoke PRIVATE cxx_std_20)

add_test(NAME smoke COMMAND tiles_renderer_smoke)
