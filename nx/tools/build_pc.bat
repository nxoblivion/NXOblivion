@echo off
rem Configures and builds nx\app for Windows (the SDK fork in nx\sdk is built from source as part of it, and the
rem code generator re-runs by itself when its inputs change).
rem Usage: nx\tools\build_pc.bat [preset] [extra cmake --build args]   (default preset win-amd64-release)
rem Output: nx\app\out\build\<preset>\oblivion.exe
call "%~dp0pc_env.bat" || exit /b 1
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=win-amd64-release"
pushd "%~dp0..\app" || exit /b 1
if not exist "out\build\%PRESET%\CMakeCache.txt" (
  cmake --preset %PRESET% || (popd & exit /b 1)
)
cmake --build "out\build\%PRESET%" %2 %3 %4 %5
set RC=%ERRORLEVEL%
popd
exit /b %RC%
