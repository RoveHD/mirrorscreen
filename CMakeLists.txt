cmake_minimum_required(VERSION 3.21)

project(DisplayMirror
        VERSION 1.0.0
        DESCRIPTION "Mirror one display onto another with independent resolution and refresh rate"
        LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DisplayMirror targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(DisplayMirror WIN32
    src/main.cpp
    src/displays.cpp
    src/capture.cpp
    src/renderer.cpp
    src/mirror.cpp
    src/settings.cpp
    src/log.cpp
    src/displays.h
    src/capture.h
    src/renderer.h
    src/mirror.h
    src/settings.h
    src/log.h
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
    ole32
    wtsapi32
)

if(MSVC)
    target_compile_options(DisplayMirror PRIVATE /W4 /permissive- /utf-8)
    # The config window uses the v6 common controls for a native look.
    target_link_options(DisplayMirror PRIVATE
        "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
    set_property(TARGET DisplayMirror PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # MinGW is not a supported target, but keeping it building is a cheap way
    # to compile-check the code outside Visual Studio. -municode selects the
    # wWinMain entry point that MSVC picks up automatically.
    target_compile_options(DisplayMirror PRIVATE -Wall -Wextra)
    target_link_options(DisplayMirror PRIVATE -municode)
endif()

install(TARGETS DisplayMirror RUNTIME DESTINATION bin)
