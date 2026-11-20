@echo off
rem Windows host toolchain for the nx/ (reNut port) build tree, after katamari-nx's tools/pc_env.bat:
rem   - MSVC headers/libs + Windows SDK from VS 18 Community (vcvars64)
rem   - LLVM clang/clang++ (on PATH) driving the MSVC ABI, linked with lld-link
rem   - CMake and Ninja from PATH
rem Usage: call nx\tools\pc_env.bat, then run cmake with %REX_CMAKE_COMPILERS%.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set REX_CMAKE_COMPILERS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_LINKER_TYPE=LLD "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2"
rem The SDK fork's VFS index (sdk/src/filesystem/devices/host_path_index.cpp) writes
rem <parent of the game folder>\cache\vfs_index_assets.bin. nx\app\assets is a junction to the repository's assets\,
rem so that would land in the repository root, outside nx\. Off for every tool run from this environment (the codegen
rem step of the build included): a full scan instead (about 2 ms for Oblivion's 199 entries).
set REX_VFS_INDEX=false
