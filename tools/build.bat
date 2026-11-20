@echo off
rem Configure and build the game. Usage: tools\build.bat [preset] (default local-release)
set PRESET=%1
if "%PRESET%"=="" set PRESET=local-release
cd /d "%~dp0.."
call "%~dp0vsenv.bat" cmake --preset %PRESET% || exit /b 1
call "%~dp0vsenv.bat" cmake --build out/build/%PRESET% || exit /b 1
