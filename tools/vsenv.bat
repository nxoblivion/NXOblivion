@echo off
rem Runs the given command inside a VS x64 developer environment.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
%*
