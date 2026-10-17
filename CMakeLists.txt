cmake_minimum_required(VERSION 3.20)
project(Lumen LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the executable runs on any Windows 10/11 machine without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB_RECURSE LUMEN_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(Lumen WIN32 ${LUMEN_SOURCES} res/app.rc)

target_include_directories(Lumen PRIVATE src)
target_compile_definitions(Lumen PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(Lumen PRIVATE /utf-8 /permissive- /W3 /Zc:__cplusplus)
target_link_options(Lumen PRIVATE /MANIFEST:NO)  # the manifest is embedded by res/app.rc
target_link_libraries(Lumen PRIVATE
    d2d1 d3d11 dxgi dxguid dwrite windowscodecs winhttp xinput
    shell32 shlwapi ole32 oleaut32 uuid user32 gdi32 advapi32 crypt32 version dwmapi winmm mfplat mfreadwrite mfuuid)
