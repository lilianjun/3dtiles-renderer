# install_smoke (P14)

CI-only fixture that locks in P13's `cmake --install` packaging: it proves an
external project can `find_package(tiles_renderer CONFIG)`, link
`tiles_renderer::tiles_renderer`, and run.

It is **not** part of ctest (not registered in `tests/CMakeLists.txt`).

## What it checks

`main.cpp` calls only `tiles_renderer::Renderer::version()` and prints it.
`version()` is a pure function — no display, no Filament engine, no tileset —
so the smoke binary runs anywhere, including a headless CI runner without
X. Rendering correctness is already covered by the headless ctest suite
(Mesa + xvfb); duplicating it here would add nothing.

## How CI uses it (linux job)

1. `cmake --install build/linux --prefix $RUNNER_TEMP/tiles_sdk`
2. `cmake -S tests/install_smoke -B <tmp>/smoke-build -DCMAKE_PREFIX_PATH=$RUNNER_TEMP/tiles_sdk`
3. `cmake --build <tmp>/smoke-build`
4. Run the binary; it must print `install_smoke: sdk version: <non-empty>`.
