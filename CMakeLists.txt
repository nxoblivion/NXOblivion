# oblivion - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(oblivion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(OBLIVION_SOURCES
    src/main.cpp
)

# Census of the game's Direct3D calls for the reNut port (docs/renut-port.md); PC test aid.
option(OBLIVION_D3D_TRACE "Wrap the game's Direct3D functions with the call trace (OBLIVION_D3D_TRACE=1 at run time)" OFF)
if(OBLIVION_D3D_TRACE)
    list(APPEND OBLIVION_SOURCES src/renut/d3d_trace.cpp)
endif()

if(WIN32)
    add_executable(oblivion WIN32 ${OBLIVION_SOURCES})
else()
    add_executable(oblivion ${OBLIVION_SOURCES})
endif()

rexglue_setup_target(oblivion GPU_PLUGINS xenos)

# Emit a PDB (also for Release) so crash backtraces can be symbolized.
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_link_options(oblivion PRIVATE -Wl,/DEBUG)
endif()
