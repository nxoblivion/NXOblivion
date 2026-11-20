# oblivion (nx) - The Elder Scrolls IV: Oblivion (Xbox 360), ReXGlue static recompilation with reNut's
# Direct3D-level renderer (docs/renut-port.md). GPL-3.0.
#
# Derived from ChanseyIsTheBest's katamari-nx app/CMakeLists.txt (dcd7e290, GPL-3.0), stripped to what Oblivion
# needs: the SDK fork (nx/sdk, REXSDK_DIR) built from source, the generated game code, a minimal app (src/main.cpp,
# src/oblivion_app.h) and reNut's renderer (src/renut_engine/gpu, cmake/banjo_renut_gpu.cmake) compiled and linked in
# with none of its hooks registered (OBLIVION_RENUT_HOOKS OFF). The game runs on the SDK's emulated Xenos GPU
# (oblivion_renderer = "emulated", the default). Not taken from katamari-nx: its post-codegen passes and their
# BANJO_GEN_* switches, the Switch LTO / PGO options, banjo-nx's PM4 renderer (src/me), nfsmw-nx's renderer
# (src/native), the hot-function shards (src/cpu), the profilers, DLC, title update and the game's keys.
# nx/docs/bootstrap-notes.md has the reasons.

cmake_minimum_required(VERSION 3.25)
project(oblivion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# katamari-nx: -mearly-ra=none / -mno-outline-atomics for everything compiled for the console (GCC miscompiles, see
# katamari-nx's app/CMakeLists.txt). Kept for the later Switch build; nothing on the PC.
option(BANJO_SWITCH_EARLY_RA "Leave GCC's AArch64 early register allocation pass on (Switch; miscompiles)" OFF)
if(REXGLUE_PLATFORM_SWITCH AND NOT BANJO_SWITCH_EARLY_RA)
    add_compile_options(-mearly-ra=none)
    add_link_options(-mearly-ra=none)
endif()
if(REXGLUE_PLATFORM_SWITCH)
    add_compile_options(-mno-outline-atomics)
endif()

include(generated/rexglue.cmake)

set(OBLIVION_SOURCES
    src/main.cpp
    # The keys and entry points of katamari-nx files this tree does not build, which reNut's renderer reads.
    src/oblivion_keys.cpp
    # The Switch's clocks while the game runs (switch_clocks; nothing on the PC).
    src/oblivion_switch_clocks.cpp
    # The mouse turns the player by itself, as on the PC (the manifest's mid-function hook in the look update).
    src/oblivion_mouse_look.cpp
)

# The root build's census of the game's Direct3D calls (docs/renut-port.md, ../../src/renut/d3d_trace.cpp), here for
# in-game runs with input_script; PC test aid, active with the environment variable OBLIVION_D3D_TRACE=1.
option(OBLIVION_D3D_TRACE "Wrap the game's Direct3D functions with the call trace" OFF)
if(OBLIVION_D3D_TRACE)
    list(APPEND OBLIVION_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../../src/renut/d3d_trace.cpp)
endif()

if(WIN32)
    add_executable(oblivion WIN32 ${OBLIVION_SOURCES})
else()
    add_executable(oblivion ${OBLIVION_SOURCES})
endif()

# GPU_PLUGINS stages the emulated-GPU backend (rexgpu-xenos.dll) next to the exe: the renderer of this build.
rexglue_setup_target(oblivion GPU_PLUGINS xenos)

# reNut's Direct3D-level renderer, compiled and linked in; chosen at boot with oblivion_renderer = "renut" once its
# hooks are at Oblivion's functions (src/renut_engine/gpu/banjo_renut_select.h).
option(OBLIVION_RENUT_RENDERER "Build reNut's renderer in (oblivion_renderer = renut)" ON)
if(OBLIVION_RENUT_RENDERER)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/banjo_renut_gpu.cmake")
endif()

# katamari-nx: the SDK compiles its own targets with these; the app's and the game code's get them too, so the game's
# floating point is not contracted into FMAs (GCC's default on the Switch) and type-punned guest accesses do not rely
# on strict aliasing.
foreach(_target oblivion oblivion_recomp)
    if(TARGET ${_target})
        target_compile_options(${_target} PRIVATE -ffp-contract=off -fno-strict-aliasing)
    endif()
endforeach()
# katamari-nx: the generated game code without errno bookkeeping in math functions (IEEE results unchanged).
if(TARGET oblivion_recomp AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(oblivion_recomp PRIVATE -fno-math-errno)
endif()

# The app includes imgui.h and xxhash.h directly; when the SDK is built from source (REXSDK_DIR) they are private to
# rexruntime.
foreach(_dep imgui xxhash renderdoc)
    if(TARGET ${_dep})
        target_include_directories(oblivion PRIVATE $<TARGET_PROPERTY:${_dep},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()

# Nintendo Switch (after katamari-nx's app/CMakeLists.txt): oblivion.nro next to the ELF, built in devkitPro's container
# (tools/build_switch.sh). On the SD card: sdmc:/switch/reoblivion/reoblivion.nro, with the extracted disc in
# game_root/ beside it or in the emulated-GPU port's sdmc:/oblivion/assets (src/oblivion_app.h).
if(REXGLUE_PLATFORM_SWITCH)
    # Ryujinx-style loaders start NROs without argv[0]; the SDK then takes the NRO's usual place on the SD.
    if(TARGET rexcore)
        target_compile_definitions(rexcore PRIVATE REX_SWITCH_FALLBACK_EXE="/switch/reoblivion/reoblivion.nro")
    endif()
    target_link_options(oblivion PRIVATE "LINKER:-Map=${CMAKE_BINARY_DIR}/oblivion.map")
    # The fixed settings compiled in, the player's oblivion.toml written next to the NRO when missing.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/oblivion_switch_defaults.toml" OBLIVION_BUILTIN_TEXT)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/oblivion_switch.toml" OBLIVION_PLAYER_TEXT)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/oblivion_builtin_config.h.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/builtin_config/oblivion_builtin_config.h" @ONLY)
    target_include_directories(oblivion PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_config")
    target_compile_definitions(oblivion PRIVATE OBLIVION_BUILTIN_CONFIG=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/oblivion_switch_defaults.toml"
                 "${CMAKE_CURRENT_SOURCE_DIR}/oblivion_switch.toml")
    rexglue_switch_add_nro(oblivion
        NAME "NXOblivion"
        AUTHOR "nxoblivion, after reNut (masterspike52) and katamari-nx (ChanseyIsTheBest)"
        VERSION "0.1.22")
endif()

# reOblivion web (2026-11-17, docs/web-port.md): oblivion.html/.js/.wasm for the browser. main() runs on a worker
# (PROXY_TO_PTHREAD), so the game may block; the page's thread stays free for the canvas, input and audio. The guest
# window (4.5 GB, memory_web.cpp) needs 64-bit memory that grows: the browser commits it as it is touched.
if(REXGLUE_PLATFORM_WEB)
    set_target_properties(oblivion PROPERTIES SUFFIX ".html")
    # The disc over HTTP (src/web/web_game_files.cpp): a WASMFS backend, built on Emscripten's own WASMFS classes.
    target_sources(oblivion PRIVATE src/web/web_game_files.cpp)
    target_include_directories(oblivion PRIVATE "${EMSCRIPTEN_ROOT_PATH}/system/lib/wasmfs")
    # The fixed settings compiled in (keyboard and mouse on), the player's /save/oblivion.toml written when missing.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/oblivion_web_defaults.toml" OBLIVION_BUILTIN_TEXT)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/oblivion_web.toml" OBLIVION_PLAYER_TEXT)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/oblivion_builtin_config.h.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/builtin_config/oblivion_builtin_config.h" @ONLY)
    target_include_directories(oblivion PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_config")
    target_compile_definitions(oblivion PRIVATE OBLIVION_BUILTIN_CONFIG=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/oblivion_web_defaults.toml" "${CMAKE_CURRENT_SOURCE_DIR}/oblivion_web.toml")
    target_link_options(oblivion PRIVATE
        -sPROXY_TO_PTHREAD=1
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=512MB
        -sMAXIMUM_MEMORY=16GB
        # Guest threads start while the game runs; a worker for each must exist already (a thread cannot wait for
        # the page to make one).
        -sPTHREAD_POOL_SIZE=64
        -sPTHREAD_POOL_SIZE_STRICT=0
        -sSTACK_SIZE=8MB
        -sDEFAULT_PTHREAD_STACK_SIZE=4MB
        -sMALLOC=mimalloc
        -sENVIRONMENT=web,worker
        -sEXIT_RUNTIME=0
        -sFORCE_FILESYSTEM=1
        -sWASMFS=1
        -sASSERTIONS=1
        # No OFFSCREENCANVAS_SUPPORT: with it the canvas went to main()'s worker, and the page's thread draws on it
        # (web/plume/webgpu_server.js).
        -sEXPORTED_RUNTIME_METHODS=FS,callMain
        "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/../web/shell.html"
        -g2)
    set_property(TARGET oblivion APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../web/shell.html")
endif()

# PC: line tables (CodeView) and a PDB, so crashes can be named with llvm-symbolizer. Debug info only.
if(WIN32)
    target_compile_options(oblivion PRIVATE -gline-tables-only -gcodeview)
    target_link_options(oblivion PRIVATE "LINKER:/DEBUG:FULL")
endif()
