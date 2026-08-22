@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d %~dp0
if "%~1"=="" (
    cmake --preset msvc-release
    cmake --build --preset msvc-release
) else (
    cmake --preset %~1
    cmake --build --preset %~1
)
echo BUILD_EXIT=%ERRORLEVEL%
