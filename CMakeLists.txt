cmake_minimum_required(VERSION 3.21)

project(DisplayMirror
        VERSION 1.0.0
        DESCRIPTION "Mirror one display onto another with independent resolution and refresh rate"
        LANGUAGES CXX)

# The .rc carries the application and tray icon.
if(WIN32)
    enable_language(RC)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "DisplayMirror targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(DisplayMirror WIN32
    src/main.cpp
    src/app.rc
    src/displays.cpp
    src/capture.cpp
    src/renderer.cpp
    src/mirror.cpp
    src/settings.cpp
    src/theme.cpp
    src/updater.cpp
    src/log.cpp
    src/displays.h
    src/capture.h
    src/renderer.h
    src/mirror.h
    src/settings.h
    src/theme.h
    src/updater.h
    src/version.h
    src/log.h
    src/resource.h
    src/app.manifest
)

target_compile_definitions(DisplayMirror PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# Everything comes from the Windows SDK; there are no third-party dependencies.
target_link_libraries(DisplayMirror PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dxguid
    user32
    gdi32
    comctl32
    shell32
    dwmapi
    uxtheme
    gdiplus
    winhttp
    ole32
    wtsapi32
)

if(MSVC)
    target_compile_options(DisplayMirror PRIVATE /W4 /permissive- /utf-8)
    # src/app.manifest is embedded through the .rc for every toolchain, so the
    # linker must not generate a second, competing one.
    target_link_options(DisplayMirror PRIVATE "/MANIFEST:NO")
    set_property(TARGET DisplayMirror PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # MinGW is not a supported target, but keeping it building is a cheap way
    # to compile-check the code outside Visual Studio. -municode selects the
    # wWinMain entry point that MSVC picks up automatically.
    target_compile_options(DisplayMirror PRIVATE -Wall -Wextra)
    target_link_options(DisplayMirror PRIVATE -municode)
endif()

install(TARGETS DisplayMirror RUNTIME DESTINATION bin)
