#pragma once
// App: owns the Win32 window, the dedicated render thread, and wires
// together DeviceEnum -> CaptureEngine -> D3DRenderer.
//
// The render loop runs on its own thread, separate from the Win32 message
// pump, and busy-polls CaptureEngine::TryGetLatestFrame() at a tight
// interval rather than waiting on a fixed-rate timer. This means a new
// frame gets to the screen as soon as it's available instead of waiting
// for the next scheduled tick.

#include <windows.h>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include "CaptureEngine.h"
#include "D3DRenderer.h"
#include "DeviceEnum.h"
#include "ScreenCapture.h"

class App {
public:
    int Run(HINSTANCE hInstance, int nCmdShow);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    void CreateMainWindow(HINSTANCE hInstance, int nCmdShow);
    void BuildDeviceMenu();
    void BuildFormatMenu();
    void OpenDevice(size_t index);
    void SelectFormat(size_t index);
    void ToggleFullscreen();
    void RenderThreadProc();

    void UpdateColorMenu();

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;

    CaptureEngine m_capture;
    ScreenCapture m_screenCapture;
    D3DRenderer m_renderer;

    std::vector<CaptureDeviceInfo> m_devices;
    std::vector<FormatOption> m_formats;
    FormatOption m_activeFormat;
    bool m_deviceOpen = false;
    bool m_isScreenCapture = false;

    std::thread m_renderThread;
    std::atomic<bool> m_running{false};

    bool m_keepAspect = true;
    bool m_fullscreen = false;
    WINDOWPLACEMENT m_prevPlacement{sizeof(WINDOWPLACEMENT)};
    DWORD m_prevStyle = 0;

    HMENU m_menuBar = nullptr;
    HMENU m_deviceMenu = nullptr;
    HMENU m_formatMenu = nullptr;
    HMENU m_colorMenu = nullptr;
};
