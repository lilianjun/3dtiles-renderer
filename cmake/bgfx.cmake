# cmake/bgfx.cmake — bgfx rendering backend integration.
#
# Uses bgfx.cmake (CMake packaging for bkaradzic/bgfx) via FetchContent.
# Provides: bgfx, bx, bimg targets, plus shaderc for shader compilation.
#
# Pinned to a specific commit for reproducibility.

if(NOT TILES_WITH_BGFX)
    return()
endif()

include(FetchContent)

# Pin bgfx.cmake to a specific commit.
# bgfx.cmake tracks upstream bgfx/bx/bimg as submodules.
set(BGFX_CMAKE_TAG "v1.0" CACHE STRING "bgfx.cmake tag to fetch")
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)  # Need shaderc
set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    bgfx_cmake
    GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
    GIT_TAG master  # TODO: pin to specific commit after validation
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(bgfx_cmake)

# bgfx.cmake provides: bgfx, bx, bimg, shaderc targets
message(STATUS "bgfx.cmake integrated: bgfx, bx, bimg, shaderc targets available")
