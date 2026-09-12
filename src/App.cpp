#include "App.h"
#include <mfapi.h>
#include <shellapi.h>
#include <sstream>

namespace {
constexpr UINT ID_DEVICE_BASE = 1000;
constexpr UINT ID_FORMAT_BASE = 2000;
constexpr UINT ID_TOGGLE_ASPECT = 3001;
constexpr UINT ID_TOGGLE_VSYNC = 3002;
constexpr UINT ID_TOGGLE_FULLSCREEN = 3003;
constexpr UINT ID_ABOUT = 3004;
constexpr UINT ID_MIRROR_FLIP = 3005;
constexpr UINT ID_RANGE_LIMITED = 3006;
constexpr UINT ID_RANGE_FULL = 3007;
constexpr UINT ID_BRIGHTNESS_INC = 3008;
constexpr UINT ID_BRIGHTNESS_DEC = 3009;
constexpr UINT ID_CONTRAST_INC = 3010;
constexpr UINT ID_CONTRAST_DEC = 3011;
constexpr UINT ID_SATURATION_INC = 3012;
constexpr UINT ID_SATURATION_DEC = 3013;
constexpr UINT ID_RESET_COLORS = 3014;
const wchar_t* kClassName = L"InstantDisplayWnd";
}

int App::Run(HINSTANCE hInstance, int nCmdShow) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return -1;
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { CoUninitialize(); return -1; }

    m_hInstance = hInstance;
    CreateMainWindow(hInstance, nCmdShow);

    m_devices = EnumerateCaptureDevices();
    BuildDeviceMenu();
    if (!m_devices.empty()) {
        OpenDevice(0); // auto-open the first device; user can switch via menu
    }

    m_running = true;
    m_renderThread = std::thread(&App::RenderThreadProc, this);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    m_running = false;
    if (m_renderThread.joinable()) m_renderThread.join();
    m_screenCapture.Stop();
    m_capture.Stop();

    MFShutdown();
    CoUninitialize();
    return (int)msg.wParam;
}

void App::CreateMainWindow(HINSTANCE hInstance, int nCmdShow) {
    WNDCLASSEX wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS; // CS_DBLCLKS enables WM_LBUTTONDBLCLK
    wc.lpfnWndProc = &App::WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    wc.hIconSm = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kClassName;
    RegisterClassEx(&wc);

    m_menuBar = CreateMenu();
    m_deviceMenu = CreatePopupMenu();
    m_formatMenu = CreatePopupMenu();
    m_colorMenu = CreatePopupMenu();
    AppendMenu(m_menuBar, MF_POPUP, (UINT_PTR)m_deviceMenu, L"Device");
    AppendMenu(m_menuBar, MF_POPUP, (UINT_PTR)m_formatMenu, L"Format");

    HMENU viewMenu = CreatePopupMenu();
    AppendMenu(viewMenu, MF_STRING, ID_TOGGLE_ASPECT, L"Keep Aspect Ratio\tA");
    AppendMenu(viewMenu, MF_STRING, ID_TOGGLE_VSYNC, L"VSync (adds latency)\tV");
    AppendMenu(viewMenu, MF_STRING, ID_TOGGLE_FULLSCREEN, L"Fullscreen\tF11");
    AppendMenu(m_menuBar, MF_POPUP, (UINT_PTR)viewMenu, L"View");

    // Color Menu
    AppendMenu(m_colorMenu, MF_STRING, ID_RANGE_LIMITED, L"Limited Range (16-235, Standard TV/HDMI)\tC");
    AppendMenu(m_colorMenu, MF_STRING, ID_RANGE_FULL, L"Full Range (0-255, PC/Monitor)\tC");
    AppendMenu(m_colorMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(m_colorMenu, MF_STRING, ID_MIRROR_FLIP, L"Mirror Horizontal (Webcam Mode)\tM");
    AppendMenu(m_colorMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(m_colorMenu, MF_STRING, ID_BRIGHTNESS_INC, L"Increase Brightness (+5%)\tCtrl+Up");
    AppendMenu(m_colorMenu, MF_STRING, ID_BRIGHTNESS_DEC, L"Decrease Brightness (-5%)\tCtrl+Down");
    AppendMenu(m_colorMenu, MF_STRING, ID_CONTRAST_INC, L"Increase Contrast (+10%)\tCtrl+Right");
    AppendMenu(m_colorMenu, MF_STRING, ID_CONTRAST_DEC, L"Decrease Contrast (-10%)\tCtrl+Left");
    AppendMenu(m_colorMenu, MF_STRING, ID_SATURATION_INC, L"Increase Saturation (+10%)\tAlt+Right");
    AppendMenu(m_colorMenu, MF_STRING, ID_SATURATION_DEC, L"Decrease Saturation (-10%)\tAlt+Left");
    AppendMenu(m_colorMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(m_colorMenu, MF_STRING, ID_RESET_COLORS, L"Reset Colors to Default\tR");
    AppendMenu(m_menuBar, MF_POPUP, (UINT_PTR)m_colorMenu, L"Color");

    HMENU helpMenu = CreatePopupMenu();
    AppendMenu(helpMenu, MF_STRING, ID_ABOUT, L"About InstantDisplay v1.2.0");
    AppendMenu(m_menuBar, MF_POPUP, (UINT_PTR)helpMenu, L"Help");

    m_hwnd = CreateWindowEx(0, kClassName, L"InstantDisplay v1.2.0", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
                             nullptr, m_menuBar, hInstance, this);
    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
    ShowWindow(m_hwnd, nCmdShow);

    HRESULT hr = m_renderer.Init(m_hwnd);
    if (FAILED(hr)) {
        MessageBox(m_hwnd, L"Failed to initialize Direct3D11 renderer.", L"Error", MB_ICONERROR);
    }
    UpdateColorMenu();
}

void App::UpdateColorMenu() {
    if (!m_colorMenu) return;
    const auto& params = m_renderer.GetColorParams();
    CheckMenuItem(m_colorMenu, ID_RANGE_LIMITED, params.colorRange == 0 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m_colorMenu, ID_RANGE_FULL, params.colorRange == 1 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m_colorMenu, ID_MIRROR_FLIP, m_renderer.IsMirrored() ? MF_CHECKED : MF_UNCHECKED);

    std::wstringstream ss;
    ss << L"InstantDisplay v1.2.0";
    if (m_deviceOpen) {
        ss << L" - " << m_activeFormat.Describe();
    }
    if (params.brightness != 0.0f || params.contrast != 1.0f || params.saturation != 1.0f) {
        wchar_t buf[64];
        swprintf_s(buf, L" [B: %+.2f, C: %.2f, S: %.2f]", params.brightness, params.contrast, params.saturation);
        ss << buf;
    }
    if (params.colorRange == 1) {
        ss << L" [Full Range]";
    }
    if (m_renderer.IsMirrored()) {
        ss << L" [Mirrored]";
    }
    SetWindowText(m_hwnd, ss.str().c_str());
}

void App::BuildDeviceMenu() {
    while (GetMenuItemCount(m_deviceMenu) > 0) DeleteMenu(m_deviceMenu, 0, MF_BYPOSITION);
    if (m_devices.empty()) {
        AppendMenu(m_deviceMenu, MF_STRING | MF_GRAYED, 0, L"(no capture devices found)");
        return;
    }
    for (size_t i = 0; i < m_devices.size(); ++i) {
        AppendMenu(m_deviceMenu, MF_STRING, ID_DEVICE_BASE + (UINT)i, m_devices[i].friendlyName.c_str());
    }
}

void App::BuildFormatMenu() {
    while (GetMenuItemCount(m_formatMenu) > 0) DeleteMenu(m_formatMenu, 0, MF_BYPOSITION);
    for (size_t i = 0; i < m_formats.size(); ++i) {
        AppendMenu(m_formatMenu, MF_STRING, ID_FORMAT_BASE + (UINT)i, m_formats[i].Describe().c_str());
    }
}

void App::OpenDevice(size_t index) {
    if (index >= m_devices.size()) return;
    m_capture.Stop();
    m_screenCapture.Stop();
    m_deviceOpen = false;
    m_isScreenCapture = false;

    if (m_devices[index].isScreenCapture) {
        m_formats.clear();
        m_screenCapture.Start(m_devices[index].monitorIndex, 60);

        FormatOption opt;
        opt.width = m_screenCapture.GetWidth();
        opt.height = m_screenCapture.GetHeight();
        opt.fpsNumerator = 60;
        opt.fpsDenominator = 1;
        opt.format = PixelFormat::RGB32;
        m_formats.push_back(opt);

        BuildFormatMenu();
        m_activeFormat = opt;
        m_isScreenCapture = true;
        m_deviceOpen = true;
        UpdateColorMenu();
        return;
    }

    m_formats.clear();
    HRESULT hr = m_capture.Open(m_devices[index].symbolicLink, &m_formats);
    if (FAILED(hr)) {
        MessageBox(m_hwnd, L"Failed to open capture device.", L"Error", MB_ICONERROR);
        return;
    }
    BuildFormatMenu();

    if (m_formats.empty()) return;

    // Prefer NV12/YUY2 (no CPU decode) over MJPEG at the highest resolution
    // available; fall back to MJPEG only if nothing else exists.
    size_t best = 0;
    auto rank = [](PixelFormat f) { return f == PixelFormat::NV12 ? 0 : f == PixelFormat::YUY2 ? 1 : 2; };
    for (size_t i = 1; i < m_formats.size(); ++i) {
        if (rank(m_formats[i].format) < rank(m_formats[best].format)) best = i;
        else if (rank(m_formats[i].format) == rank(m_formats[best].format) &&
                 (uint64_t)m_formats[i].width * m_formats[i].height >
                 (uint64_t)m_formats[best].width * m_formats[best].height) best = i;
    }
    SelectFormat(best);
}

void App::SelectFormat(size_t index) {
    if (index >= m_formats.size()) return;
    if (m_isScreenCapture) {
        m_activeFormat = m_formats[index];
        UpdateColorMenu();
        return;
    }
    HRESULT hr = m_capture.StartStream(m_formats[index]);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"Failed to start capture with selected format (0x" << std::hex << (uint32_t)hr << L").";
        MessageBox(m_hwnd, ss.str().c_str(), L"Error", MB_ICONERROR);
        return;
    }
    m_activeFormat = m_formats[index];
    m_deviceOpen = true;
    UpdateColorMenu();
}

void App::ToggleFullscreen() {
    m_fullscreen = !m_fullscreen;
    if (m_fullscreen) {
        GetWindowPlacement(m_hwnd, &m_prevPlacement);
        m_prevStyle = (DWORD)GetWindowLongPtr(m_hwnd, GWL_STYLE);
        SetWindowLongPtr(m_hwnd, GWL_STYLE, m_prevStyle & ~(WS_CAPTION | WS_THICKFRAME));
        SetMenu(m_hwnd, nullptr);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfo(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtr(m_hwnd, GWL_STYLE, m_prevStyle);
        SetMenu(m_hwnd, m_menuBar);
        SetWindowPlacement(m_hwnd, &m_prevPlacement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void App::RenderThreadProc() {
    CapturedFrame frame;
    while (m_running) {
        bool gotFrame = false;
        if (m_deviceOpen) {
            if (m_isScreenCapture) {
                gotFrame = m_screenCapture.TryGetLatestFrame(frame);
            } else {
                gotFrame = m_capture.TryGetLatestFrame(frame);
            }
        }

        if (gotFrame) {
            frame.width = m_activeFormat.width;
            frame.height = m_activeFormat.height;
            m_renderer.RenderFrame(frame, m_activeFormat.width, m_activeFormat.height, m_keepAspect);
        } else {
            Sleep(1);
        }
    }
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: {
            UINT w = LOWORD(lParam), h = HIWORD(lParam);
            if (w > 0 && h > 0) m_renderer.Resize(w, h);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            ToggleFullscreen();
            return 0;
        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

            if (wParam == VK_F11) ToggleFullscreen();
            else if (wParam == VK_ESCAPE && m_fullscreen) ToggleFullscreen();
            else if (wParam == 'A' && !ctrl && !alt) m_keepAspect = !m_keepAspect;
            else if (wParam == 'V' && !ctrl && !alt) m_renderer.kVsync = !m_renderer.kVsync;
            else if (wParam == 'M' && !ctrl && !alt) {
                m_renderer.ToggleMirror();
                UpdateColorMenu();
            } else if (wParam == 'C' && !ctrl && !alt) {
                int nextRange = (m_renderer.GetColorParams().colorRange == 0) ? 1 : 0;
                m_renderer.SetColorRange(nextRange);
                UpdateColorMenu();
            } else if (wParam == 'R' && !ctrl && !alt) {
                m_renderer.ResetColorParams();
                UpdateColorMenu();
            } else if (ctrl && wParam == VK_UP) {
                m_renderer.AdjustBrightness(+0.05f);
                UpdateColorMenu();
            } else if (ctrl && wParam == VK_DOWN) {
                m_renderer.AdjustBrightness(-0.05f);
                UpdateColorMenu();
            } else if (ctrl && wParam == VK_RIGHT) {
                m_renderer.AdjustContrast(+0.1f);
                UpdateColorMenu();
            } else if (ctrl && wParam == VK_LEFT) {
                m_renderer.AdjustContrast(-0.1f);
                UpdateColorMenu();
            } else if (alt && wParam == VK_RIGHT) {
                m_renderer.AdjustSaturation(+0.1f);
                UpdateColorMenu();
            } else if (alt && wParam == VK_LEFT) {
                m_renderer.AdjustSaturation(-0.1f);
                UpdateColorMenu();
            }
            return 0;
        }
        case WM_COMMAND: {
            UINT id = LOWORD(wParam);
            if (id >= ID_DEVICE_BASE && id < ID_DEVICE_BASE + m_devices.size()) {
                OpenDevice(id - ID_DEVICE_BASE);
            } else if (id >= ID_FORMAT_BASE && id < ID_FORMAT_BASE + m_formats.size()) {
                SelectFormat(id - ID_FORMAT_BASE);
            } else if (id == ID_TOGGLE_ASPECT) {
                m_keepAspect = !m_keepAspect;
            } else if (id == ID_TOGGLE_VSYNC) {
                m_renderer.kVsync = !m_renderer.kVsync;
            } else if (id == ID_TOGGLE_FULLSCREEN) {
                ToggleFullscreen();
            } else if (id == ID_RANGE_LIMITED) {
                m_renderer.SetColorRange(0);
                UpdateColorMenu();
            } else if (id == ID_RANGE_FULL) {
                m_renderer.SetColorRange(1);
                UpdateColorMenu();
            } else if (id == ID_MIRROR_FLIP) {
                m_renderer.ToggleMirror();
                UpdateColorMenu();
            } else if (id == ID_RESET_COLORS) {
                m_renderer.ResetColorParams();
                UpdateColorMenu();
            } else if (id == ID_BRIGHTNESS_INC) {
                m_renderer.AdjustBrightness(+0.05f);
                UpdateColorMenu();
            } else if (id == ID_BRIGHTNESS_DEC) {
                m_renderer.AdjustBrightness(-0.05f);
                UpdateColorMenu();
            } else if (id == ID_CONTRAST_INC) {
                m_renderer.AdjustContrast(+0.1f);
                UpdateColorMenu();
            } else if (id == ID_CONTRAST_DEC) {
                m_renderer.AdjustContrast(-0.1f);
                UpdateColorMenu();
            } else if (id == ID_SATURATION_INC) {
                m_renderer.AdjustSaturation(+0.1f);
                UpdateColorMenu();
            } else if (id == ID_SATURATION_DEC) {
                m_renderer.AdjustSaturation(-0.1f);
                UpdateColorMenu();
            } else if (id == ID_ABOUT) {
                MessageBox(hwnd,
                    L"InstantDisplay v1.2.0\n\n"
                    L"Ultra-Low Latency Video Capture & Display\n"
                    L"Direct3D 11 & Media Foundation\n\n"
                    L"GitHub: https://github.com/khamdaengp/instant-display",
                    L"About InstantDisplay v1.2.0", MB_ICONINFORMATION);
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
