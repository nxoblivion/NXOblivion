@echo off
rem The host tools of the deko3d shader chain for Windows (the Switch installer's kit runs them on the player's PC), into
rem nx\out\deko3d_tools\win: spirv-cross.exe (the repository SDK's SPIRV-Cross), uam.exe (nx\app\thirdparty\uam) and
rem zstd.exe (the GLSL package's dictionary, tools\deko3d_shader_package.py; XenosRecomp's zstd sources).
rem The same sources as tools\deko3d_build_tools.sh (Linux, the devkitA64 container). spirv-cross builds with LLVM's
rem clang in the MSVC environment of tools\pc_env.bat; uam's Mesa code wants POSIX threads and C99 math, so it builds
rem with MinGW-w64 gcc (POSIX threads, e.g. WinLibs; MINGW_BIN names its bin folder when gcc is not on PATH), and zstd
rem with it too. All link the C/C++ runtimes statically (no DLLs to ship).
rem Usage: nx\tools\deko3d_build_tools_win.bat
call "%~dp0pc_env.bat" || exit /b 1
set "NX=%~dp0.."
set "OUT=%NX%\out\deko3d_tools\win"
set "COMMON=-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
cmake -S "%NX%\..\thirdparty\rexglue-sdk\thirdparty\SPIRV-Cross" -B "%OUT%\spirv-cross-build" %COMMON% ^
    -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF || exit /b 1
ninja -C "%OUT%\spirv-cross-build" spirv-cross || exit /b 1
if defined MINGW_BIN set "PATH=%MINGW_BIN%;%PATH%"
set "MINGW=-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++"
cmake -S "%NX%\app\thirdparty\uam" -B "%OUT%\uam-build" %MINGW% "-DCMAKE_EXE_LINKER_FLAGS=-static -s" || exit /b 1
ninja -C "%OUT%\uam-build" uam_tool || exit /b 1
cmake -S "%NX%\app\thirdparty\reblue-XenosRecomp\thirdparty\zstd\build\cmake" -B "%OUT%\zstd-build" %MINGW% ^
    -DZSTD_BUILD_PROGRAMS=ON -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_MULTITHREAD_SUPPORT=OFF ^
    "-DCMAKE_EXE_LINKER_FLAGS=-static -s" || exit /b 1
ninja -C "%OUT%\zstd-build" zstd || exit /b 1
copy /y "%OUT%\spirv-cross-build\spirv-cross.exe" "%OUT%\spirv-cross.exe" >nul || exit /b 1
copy /y "%OUT%\uam-build\uam.exe" "%OUT%\uam.exe" >nul || exit /b 1
copy /y "%OUT%\zstd-build\programs\zstd.exe" "%OUT%\zstd.exe" >nul || exit /b 1
dir "%OUT%\*.exe"
