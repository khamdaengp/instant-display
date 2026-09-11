cmake_minimum_required(VERSION 3.20)
project(InstantDisplay CXX)

if (NOT WIN32)
    message(FATAL_ERROR "This project targets Windows 10/11 (Media Foundation + Direct3D11). "
                         "Build on Windows with MSVC / Visual Studio.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(InstantDisplay WIN32
    src/main.cpp
    src/CaptureEngine.cpp
    src/D3DRenderer.cpp
    src/DeviceEnum.cpp
    src/ScreenCapture.cpp
    src/App.cpp
    src/resource.rc
)

target_include_directories(InstantDisplay PRIVATE src)

# Core Windows media / graphics libs. All ship with the Windows SDK -
# no external dependencies to fetch or vendor.
target_link_libraries(InstantDisplay PRIVATE
    mfplat
    mfreadwrite
    mfuuid
    mf
    d3d11
    dxgi
    d3dcompiler
    user32
    gdi32
    ole32
    oleaut32
    shlwapi
    comdlg32
    winmm
)

# Favor speed; keep debug info available for latency troubleshooting.
target_compile_options(InstantDisplay PRIVATE $<$<CONFIG:Release>:/O2>)
target_compile_definitions(InstantDisplay PRIVATE UNICODE _UNICODE)
