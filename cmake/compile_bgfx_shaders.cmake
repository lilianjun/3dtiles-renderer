# cmake/compile_bgfx_shaders.cmake — Compile bgfx shaders using shaderc.
#
# Usage: include this after bgfx.cmake, then call
#   tiles_compile_bgfx_shaders(<target> <shader_dir> <output_dir>)
#
# Compiles .sc files to .bin for the current platform.

function(tiles_compile_bgfx_shaders TARGET SHADER_DIR OUTPUT_DIR)
    if(NOT TARGET shaderc)
        message(WARNING "shaderc target not available, skipping bgfx shader compilation")
        return()
    endif()

    file(GLOB SHADER_FILES "${SHADER_DIR}/*.sc")
    
    # Determine shaderc platform/profile based on current platform
    if(TILES_PLATFORM STREQUAL "linux")
        set(SHADERC_PLATFORM "linux")
        set(VS_PROFILE "spirv")  # or "glsl" for OpenGL
        set(FS_PROFILE "spirv")
    elseif(TILES_PLATFORM STREQUAL "windows")
        set(SHADERC_PLATFORM "windows")
        set(VS_PROFILE "s_5_0")
        set(FS_PROFILE "s_5_0")
    elseif(TILES_PLATFORM STREQUAL "android")
        set(SHADERC_PLATFORM "android")
        set(VS_PROFILE "spirv")
        set(FS_PROFILE "spirv")
    elseif(TILES_PLATFORM STREQUAL "ios")
        set(SHADERC_PLATFORM "ios")
        set(VS_PROFILE "metal")
        set(FS_PROFILE "metal")
    else()
        message(WARNING "Unknown platform for shaderc: ${TILES_PLATFORM}")
        return()
    endif()

    set(COMPILED_SHADERS "")
    foreach(SHADER_FILE ${SHADER_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME_WE)
        # Determine if vertex or fragment shader by suffix
        if(SHADER_NAME MATCHES "_vs$")
            set(PROFILE ${VS_PROFILE})
            set(TYPE "vertex")
        elseif(SHADER_NAME MATCHES "_fs$")
            set(PROFILE ${FS_PROFILE})
            set(TYPE "fragment")
        else()
            message(WARNING "Unknown shader type for ${SHADER_NAME}, skipping")
            continue()
        endif()

        set(OUTPUT_FILE "${OUTPUT_DIR}/${SHADER_NAME}.bin")
        
        add_custom_command(
            OUTPUT ${OUTPUT_FILE}
            COMMAND shaderc
                -f ${SHADER_FILE}
                -o ${OUTPUT_FILE}
                --type ${TYPE}
                --platform ${SHADERC_PLATFORM}
                -p ${PROFILE}
                --varyingdef ${SHADER_DIR}/varying.def.sc
            DEPENDS ${SHADER_FILE} shaderc
            COMMENT "Compiling bgfx shader ${SHADER_NAME}"
        )
        list(APPEND COMPILED_SHADERS ${OUTPUT_FILE})
    endforeach()

    # Create a custom target for all shaders
    add_custom_target(${TARGET}_shaders ALL DEPENDS ${COMPILED_SHADERS})
    add_dependencies(${TARGET} ${TARGET}_shaders)
endfunction()
