@echo off
rem Configure, build and install the bundled rexglue SDK (all configs).
cd /d "%~dp0..\thirdparty\rexglue-sdk" || exit /b 1
call "%~dp0vsenv.bat" cmake --preset win-amd64 || exit /b 1
call "%~dp0vsenv.bat" cmake --build out/build/win-amd64 --target install || exit /b 1
