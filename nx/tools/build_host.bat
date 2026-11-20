@echo off
rem Builds the SDK fork's host code generator (nx\sdk\out\win-amd64\rexglue.exe; build folder nx\sdk\out\host).
rem Usage: nx\tools\build_host.bat
call "%~dp0pc_env.bat" || exit /b 1
set "SDK=%~dp0..\sdk"
if not exist "%SDK%\out\host\CMakeCache.txt" (
  cmake -S "%SDK%" -B "%SDK%\out\host" -G Ninja -DCMAKE_BUILD_TYPE=Release %REX_CMAKE_COMPILERS% -DREXGLUE_ENABLE_TRACY=OFF || exit /b 1
)
cmake --build "%SDK%\out\host" --target rexglue || exit /b 1
