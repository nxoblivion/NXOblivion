@echo off
rem Runs the SDK fork's code generator on nx\app\oblivion_manifest.toml (output: nx\app\generated\default).
rem Usage: nx\tools\codegen.bat [log file]   (default nx\out\codegen.log)
rem The generator skips its work when the manifest and the xex match its stamp, even after the generator itself changed
rem (an SDK patch): --ignore-stamp always regenerates (unchanged files are not rewritten).
set "NX=%~dp0.."
rem No VFS index file next to the game folder (outside nx\, tools\pc_env.bat).
set REX_VFS_INDEX=false
set "LOG=%~1"
if "%LOG%"=="" set "LOG=%NX%\out\codegen.log"
if not exist "%NX%\out" mkdir "%NX%\out"
pushd "%NX%\app" || exit /b 1
"%NX%\sdk\out\win-amd64\rexglue.exe" codegen --ignore-stamp oblivion_manifest.toml > "%LOG%" 2>&1
set RC=%ERRORLEVEL%
popd
exit /b %RC%
