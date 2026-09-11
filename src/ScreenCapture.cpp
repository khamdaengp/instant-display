#include "ScreenCapture.h"
#include <chrono>
#include <sstream>
#pragma comment(lib, "winmm.lib")

namespace {

BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);
    MONITORINFOEX mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(hMon, &mi)) {
        MonitorInfo info;
        info.index = (int)list->size();
        info.rect = mi.rcMonitor;
        info.width = mi.rcMonitor.right - mi.rcMonitor.left;
        info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

        std::wstringstream ss;
        ss << L"Monitor " << (info.index + 1);
        if (info.isPrimary) ss << L" (Primary)";
        ss << L" - " << info.width << L"x" << info.height;
        info.name = ss.str();

        list->push_back(info);
    }
    return TRUE;
}

} // namespace

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> list;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&list));
    if (list.empty()) {
        // Fallback to virtual desktop
        MonitorInfo info;
        info.index = 0;
        info.rect.left = 0;
        info.rect.top = 0;
        info.width = GetSystemMetrics(SM_CXSCREEN);
        info.height = GetSystemMetrics(SM_CYSCREEN);
        info.rect.right = info.width;
        info.rect.bottom = info.height;
        info.isPrimary = true;
        info.name = L"Primary Display (" + std::to_wstring(info.width) + L"x" + std::to_wstring(info.height) + L")";
        list.push_back(info);
    }
    return list;
}

ScreenCapture::ScreenCapture() {}

ScreenCapture::~ScreenCapture() {
    Stop();
}

bool ScreenCapture::Start(int monitorIndex, UINT targetFps) {
    Stop();

    auto monitors = EnumerateMonitors();
    if (monitors.empty()) return false;
    if (monitorIndex < 0 || monitorIndex >= (int)monitors.size()) {
        monitorIndex = 0;
    }

    const auto& mon = monitors[monitorIndex];
    m_width = mon.width;
    m_height = mon.height;

    m_running = true;
    m_thread = std::thread(&ScreenCapture::CaptureThreadProc, this, mon.rect, targetFps);
    return true;
}

void ScreenCapture::Stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_hasNewFrame = false;
}

bool ScreenCapture::TryGetLatestFrame(CapturedFrame& out) {
    if (!m_hasNewFrame.exchange(false)) return false;
    std::lock_guard<std::mutex> lock(m_frameMutex);
    out = m_latestFrame;
    return true;
}

void ScreenCapture::CaptureThreadProc(RECT monitorRect, UINT targetFps) {
    timeBeginPeriod(1);

    int w = monitorRect.right - monitorRect.left;
    int h = monitorRect.bottom - monitorRect.top;
    if (w <= 0 || h <= 0) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    HGDIOBJ oldBm = SelectObject(hdcMem, hbm);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double frameIntervalSec = 1.0 / (targetFps > 0 ? targetFps : 60);

    LARGE_INTEGER nextTick;
    QueryPerformanceCounter(&nextTick);

    while (m_running) {
        // Capture screen area
        BitBlt(hdcMem, 0, 0, w, h, hdcScreen, monitorRect.left, monitorRect.top, SRCCOPY | CAPTUREBLT);

        // Capture mouse cursor
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
            POINT pt = ci.ptScreenPos;
            if (pt.x >= monitorRect.left && pt.x < monitorRect.right &&
                pt.y >= monitorRect.top && pt.y < monitorRect.bottom) {
                ICONINFO ii{};
                if (GetIconInfo(ci.hCursor, &ii)) {
                    DrawIconEx(hdcMem, pt.x - monitorRect.left - ii.xHotspot,
                               pt.y - monitorRect.top - ii.yHotspot,
                               ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
                    if (ii.hbmMask) DeleteObject(ii.hbmMask);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                }
            }
        }

        // Store latest frame
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_latestFrame.data.assign((uint8_t*)pBits, (uint8_t*)pBits + (w * h * 4));
            m_latestFrame.width = w;
            m_latestFrame.height = h;
            m_latestFrame.stride = w * 4;
            m_latestFrame.format = PixelFormat::RGB32;
            m_latestFrame.captureQpc = (uint64_t)qpc.QuadPart;
            m_hasNewFrame = true;
        }

        // Pacing for smooth target FPS
        nextTick.QuadPart += (LONGLONG)(frameIntervalSec * freq.QuadPart);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (nextTick.QuadPart > now.QuadPart) {
            double waitMs = (double)(nextTick.QuadPart - now.QuadPart) * 1000.0 / freq.QuadPart;
            if (waitMs > 1.0) {
                Sleep((DWORD)waitMs);
            }
        } else {
            // Drop behind, reset schedule
            nextTick = now;
        }
    }

    SelectObject(hdcMem, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    timeEndPeriod(1);
}
