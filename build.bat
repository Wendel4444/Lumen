@echo off
rem Lumen build script (MSVC, no CMake needed). Usage: build.bat [debug] [run]
rem Sets up the compiler environment directly (vcvars can break on PATHs containing parentheses).
setlocal enabledelayedexpansion
cd /d "%~dp0"

set CONFIG=release
set RUN=0
for %%A in (%*) do (
    if /i "%%A"=="debug" set CONFIG=debug
    if /i "%%A"=="run" set RUN=1
)

where cl >nul 2>nul
if not errorlevel 1 goto :build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :novs

set "MSVCVER="
for /f "usebackq delims=" %%v in ("%VSPATH%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt") do set "MSVCVER=%%v"
set "VCT=%VSPATH%\VC\Tools\MSVC\%MSVCVER%"
if not exist "%VCT%\bin\Hostx64\x64\cl.exe" goto :novs

set "KITS=%ProgramFiles(x86)%\Windows Kits\10"
set "SDKVER="
for /f "delims=" %%s in ('dir /b /ad /o:n "%KITS%\Include\10.*"') do (
    if exist "%KITS%\Include\%%s\um\windows.h" set "SDKVER=%%s"
)
if not defined SDKVER goto :nosdk

set "INCLUDE=%VCT%\include;%KITS%\Include\%SDKVER%\ucrt;%KITS%\Include\%SDKVER%\um;%KITS%\Include\%SDKVER%\shared;%KITS%\Include\%SDKVER%\winrt"
set "LIB=%VCT%\lib\x64;%KITS%\Lib\%SDKVER%\ucrt\x64;%KITS%\Lib\%SDKVER%\um\x64"
set "PATH=%VCT%\bin\Hostx64\x64;%KITS%\bin\%SDKVER%\x64;%PATH%"

:build
set OUT=build\%CONFIG%
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

set COMMON=/nologo /std:c++17 /utf-8 /EHsc /W3 /permissive- /Zc:__cplusplus /MP /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Isrc
if /i "%CONFIG%"=="debug" goto :cfgdebug
set FLAGS=%COMMON% /O2 /GL /MT /DNDEBUG
set LFLAGS=/LTCG /OPT:REF /OPT:ICF
goto :cfgdone
:cfgdebug
set FLAGS=%COMMON% /Od /Zi /MTd /D_DEBUG
set LFLAGS=/DEBUG
:cfgdone

set LIBS=d2d1.lib d3d11.lib dxgi.lib dxguid.lib dwrite.lib windowscodecs.lib winhttp.lib xinput.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib uuid.lib user32.lib gdi32.lib advapi32.lib crypt32.lib version.lib dwmapi.lib winmm.lib mfplat.lib mfreadwrite.lib mfuuid.lib

rc /nologo /fo "%OUT%\obj\app.res" res\app.rc
if errorlevel 1 exit /b 1

cl %FLAGS% /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" /Fe"%OUT%\Lumen.exe" src\main.cpp src\core\*.cpp src\core\providers\*.cpp src\ui\*.cpp src\app\*.cpp "%OUT%\obj\app.res" /link %LFLAGS% /SUBSYSTEM:WINDOWS %LIBS%
if errorlevel 1 exit /b 1

echo.
echo [ok] %OUT%\Lumen.exe
if "%RUN%"=="1" start "" "%OUT%\Lumen.exe"
exit /b 0

:novs
echo [erro] Visual Studio / Build Tools com C++ x64 nao encontrado.
echo        Instale "Desenvolvimento para desktop com C++" pelo Visual Studio Installer.
exit /b 1

:nosdk
echo [erro] Windows 10/11 SDK nao encontrado.
exit /b 1
