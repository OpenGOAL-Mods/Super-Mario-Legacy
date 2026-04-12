cmake_minimum_required(VERSION 3.10)

# libsm64 - Super Mario 64 as a library
# Built as a static library for integration into jak-project

set(LIBSM64_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(LIBSM64_SOURCES
    ${LIBSM64_SRC_DIR}/debug_print.c
    ${LIBSM64_SRC_DIR}/fake_interaction.c
    ${LIBSM64_SRC_DIR}/gfx_adapter.c
    ${LIBSM64_SRC_DIR}/libsm64.c
    ${LIBSM64_SRC_DIR}/load_anim_data.c
    ${LIBSM64_SRC_DIR}/load_audio_data.c
    ${LIBSM64_SRC_DIR}/load_surfaces.c
    ${LIBSM64_SRC_DIR}/load_tex_data.c
    ${LIBSM64_SRC_DIR}/obj_pool.c
    ${LIBSM64_SRC_DIR}/play_sound.c
    ${LIBSM64_SRC_DIR}/decomp/global_state.c
    ${LIBSM64_SRC_DIR}/decomp/memory.c
    ${LIBSM64_SRC_DIR}/decomp/audio/data.c
    ${LIBSM64_SRC_DIR}/decomp/audio/effects.c
    ${LIBSM64_SRC_DIR}/decomp/audio/external.c
    ${LIBSM64_SRC_DIR}/decomp/audio/globals_start.c
    ${LIBSM64_SRC_DIR}/decomp/audio/heap.c
    ${LIBSM64_SRC_DIR}/decomp/audio/load.c
    ${LIBSM64_SRC_DIR}/decomp/audio/load_dat.c
    ${LIBSM64_SRC_DIR}/decomp/audio/load_sh.c
    ${LIBSM64_SRC_DIR}/decomp/audio/playback.c
    ${LIBSM64_SRC_DIR}/decomp/audio/port_eu.c
    ${LIBSM64_SRC_DIR}/decomp/audio/port_sh.c
    ${LIBSM64_SRC_DIR}/decomp/audio/seqplayer.c
    ${LIBSM64_SRC_DIR}/decomp/audio/shindou_debug_prints.c
    ${LIBSM64_SRC_DIR}/decomp/audio/synthesis.c
    ${LIBSM64_SRC_DIR}/decomp/audio/synthesis_sh.c
    ${LIBSM64_SRC_DIR}/decomp/engine/geo_layout.c
    ${LIBSM64_SRC_DIR}/decomp/engine/graph_node.c
    ${LIBSM64_SRC_DIR}/decomp/engine/graph_node_manager.c
    ${LIBSM64_SRC_DIR}/decomp/engine/guMtxF2L.c
    ${LIBSM64_SRC_DIR}/decomp/engine/math_util.c
    ${LIBSM64_SRC_DIR}/decomp/engine/surface_collision.c
    ${LIBSM64_SRC_DIR}/decomp/game/behavior_actions.c
    ${LIBSM64_SRC_DIR}/decomp/game/interaction.c
    ${LIBSM64_SRC_DIR}/decomp/game/mario.c
    ${LIBSM64_SRC_DIR}/decomp/game/mario_actions_airborne.c
    ${LIBSM64_SRC_DIR}/decomp/game/mario_actions_automatic.c
    ${LIBSM64_SRC_DIR}/decomp/game/mario_actions_cutscene.c
    ${LIBSM64_SRC_DIR}/decomp/game/mario_actions_moving.c
    ${LIBSM64_SRC_DIR}/decomp/game/mario_actions_object.c
    ${LIBSM64_SRC_DIR}/decomp/game/mario_actions_stationary.c
    ${LIBSM64_SRC_DIR}/decomp/game/mario_actions_submerged.c
    ${LIBSM64_SRC_DIR}/decomp/game/mario_misc.c
    ${LIBSM64_SRC_DIR}/decomp/game/mario_step.c
    ${LIBSM64_SRC_DIR}/decomp/game/object_stuff.c
    ${LIBSM64_SRC_DIR}/decomp/game/platform_displacement.c
    ${LIBSM64_SRC_DIR}/decomp/game/rendering_graph_node.c
    ${LIBSM64_SRC_DIR}/decomp/game/sound_init.c
    ${LIBSM64_SRC_DIR}/decomp/mario/geo.inc.c
    ${LIBSM64_SRC_DIR}/decomp/mario/model.inc.c
    ${LIBSM64_SRC_DIR}/decomp/pc/alBnkfNew.c
    ${LIBSM64_SRC_DIR}/decomp/pc/mixer.c
    ${LIBSM64_SRC_DIR}/decomp/pc/ultra_reimplementation.c
    ${LIBSM64_SRC_DIR}/decomp/tools/convUtils.c
    ${LIBSM64_SRC_DIR}/decomp/tools/libmio0.c
    ${LIBSM64_SRC_DIR}/decomp/tools/n64graphics.c
    ${LIBSM64_SRC_DIR}/decomp/tools/utils.c
)

# Avoid duplicate stbi_* symbols when linking with jak-project's stb_image.
# Make all stb functions static/internal to n64graphics.c
set_source_files_properties(
    ${LIBSM64_SRC_DIR}/decomp/tools/n64graphics.c
    PROPERTIES COMPILE_DEFINITIONS "STB_IMAGE_STATIC;STB_IMAGE_WRITE_STATIC;stbi_zlib_compress=libsm64_stbi_zlib_compress;stbi_write_png_to_mem=libsm64_stbi_write_png_to_mem"
)

add_library(libsm64 STATIC ${LIBSM64_SOURCES})

target_compile_definitions(libsm64 PRIVATE
    SM64_LIB_EXPORT
    GBI_FLOATS
    VERSION_US
    NO_SEGMENTED_MEMORY
)

target_include_directories(libsm64 PUBLIC
    ${LIBSM64_SRC_DIR}
    ${LIBSM64_SRC_DIR}/decomp/include
)

target_include_directories(libsm64 PRIVATE
    ${LIBSM64_SRC_DIR}/decomp
    ${LIBSM64_SRC_DIR}/decomp/engine
    ${LIBSM64_SRC_DIR}/decomp/game
    ${LIBSM64_SRC_DIR}/decomp/include/PR
    ${LIBSM64_SRC_DIR}/decomp/pc
    ${LIBSM64_SRC_DIR}/decomp/audio
    ${LIBSM64_SRC_DIR}/decomp/tools
    ${LIBSM64_SRC_DIR}/decomp/mario
)

# Suppress warnings from third-party C code
if(WIN32)
    # Add shim headers for POSIX headers missing on Windows (dirent.h, strings.h, etc.)
    target_include_directories(libsm64 PRIVATE ${LIBSM64_SRC_DIR}/win32_includes)
endif()

if(MSVC)
    target_compile_options(libsm64 PRIVATE /w)
elseif(WIN32)
    # Clang on Windows (clang-cl) doesn't support -fPIC
    target_compile_options(libsm64 PRIVATE
        -w
        -fno-strict-aliasing
    )
else()
    target_compile_options(libsm64 PRIVATE
        -w
        -fno-strict-aliasing
        -fPIC
    )
endif()

if(WIN32)
    target_link_libraries(libsm64 PRIVATE ws2_32)
else()
    target_link_libraries(libsm64 PRIVATE m)
endif()
