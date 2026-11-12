@echo off
rem Builds Voxel Rethinked with the right compiler from any terminal.
rem
rem   build.cmd                  Builds the release preset
rem   build.cmd debug            Builds another preset (debug, release, relwithdebinfo, asan)
rem   build.cmd release run      Builds, then starts the game (more words go to the game)
rem   build.cmd debug test       Builds, then runs the tests
rem   build.cmd release fresh    Configures again from nothing, then builds (repairs a build folder)
rem   build.cmd release package  Builds, then puts a copy to share in out\VoxelRethinked
rem
rem docs/building.md explains all of it.
setlocal
set "preset=%~1"
if "%preset%"=="" set "preset=release"
set "action=%~2"

rem Finds the newest Visual Studio with the C++ tools. Several may be installed.
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" (
    echo Visual Studio is not installed. See docs/building.md.
    exit /b 1
)
set "vsPath="
for /f "usebackq delims=" %%P in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vsPath=%%P"
if "%vsPath%"=="" (
    echo No Visual Studio with the C++ tools found. See docs/building.md.
    exit /b 1
)
rem vcvars64 calls vswhere itself and prints an error when it is not on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%vsPath%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"

set "fresh="
if /i "%action%"=="fresh" set "fresh=--fresh"
cmake --preset %preset% %fresh% || exit /b 1
cmake --build --preset %preset% || exit /b 1

if /i "%action%"=="test" (
    ctest --preset %preset% || exit /b 1
)
if /i "%action%"=="run" (
    "build\%preset%\bin\voxel_rethinked.exe" %3 %4 %5 %6
)
if /i "%action%"=="package" (
    if exist "out\VoxelRethinked" rmdir /s /q "out\VoxelRethinked"
    mkdir "out\VoxelRethinked"
    copy /y "build\%preset%\bin\voxel_rethinked.exe" "out\VoxelRethinked\" >nul || exit /b 1
    for %%F in (shaders assets textures sounds) do (
        xcopy /e /i /q /y "build\%preset%\bin\%%F" "out\VoxelRethinked\%%F" >nul || exit /b 1
    )
    rem The C++ runtime of the compiler that made this build, for PCs without it.
    copy /y "%VCToolsRedistDir%vc_redist.x64.exe" "out\VoxelRethinked\" >nul || exit /b 1
    rem A shared or test copy keeps its worlds and settings next to the exe,
    rem never in the player's %%APPDATA%%\.voxelrethinked.
    echo This copy keeps its worlds and settings in this folder.> "out\VoxelRethinked\portable.txt"
    echo Ready to share: out\VoxelRethinked
)
