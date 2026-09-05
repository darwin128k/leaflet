@echo off
setlocal
cd /d %~dp0

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
call %VCVARS% >nul
if errorlevel 1 (
    echo Failed to init MSVC x86 environment
    exit /b 1
)

if not exist build mkdir build
cd build

cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl ..
if errorlevel 1 (
    echo CMake configure failed
    exit /b 1
)

cmake --build .
if errorlevel 1 (
    echo Build failed
    exit /b 1
)

cd ..

set ROOT=.\
if exist ..\hw.dll (
    set ROOT=..\
) else if exist ..\..\hw.dll (
    set ROOT=..\..\
)

copy /Y build\leaflet.dll "%ROOT%leaflet.dll" >nul
if errorlevel 1 (
    echo Built OK, but could not copy leaflet.dll to game root -- is the game running?
    exit /b 1
)
if exist "%ROOT%vellum.dll" del /Q "%ROOT%vellum.dll"
if exist "%ROOT%GameUI_hook.dll" del /Q "%ROOT%GameUI_hook.dll"

set ORIG_GAMEUI=%ROOT%valve\cl_dlls\GameUI_orig.dll
if exist "%ORIG_GAMEUI%" (
    copy /Y "%ORIG_GAMEUI%" "%ROOT%valve\cl_dlls\GameUI.dll" >nul
    if errorlevel 1 (
        echo Could not restore original GameUI.dll -- is the game running?
        exit /b 1
    )
)

echo Build OK: leaflet.dll
endlocal
