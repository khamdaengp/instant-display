cmake_minimum_required(VERSION 3.20)
project(LowLatencyCapture CXX)

if (NOT WIN32)
    message(FATAL_ERROR "This project targets Windows 10/11 (Media Foundation + Direct3D11). "
                         "Build on Windows with MSVC / Visual Studio.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(LowLatencyCapture WIN32
    src/main.cpp
    src/CaptureEngine.cpp
    src/D3DRenderer.cpp
    src/DeviceEnum.cpp
    src/App.cpp
    src/resource.rc
)

target_include_directories(LowLatencyCapture PRIVATE src)

# Core Windows media / graphics libs. All ship with the Windows SDK -
# no external dependencies to fetch or vendor.
target_link_libraries(LowLatencyCapture PRIVATE
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
)

# Favor speed; keep debug info available for latency troubleshooting.
target_compile_options(LowLatencyCapture PRIVATE $<$<CONFIG:Release>:/O2>)
target_compile_definitions(LowLatencyCapture PRIVATE UNICODE _UNICODE)
