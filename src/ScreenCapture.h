#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include "CaptureEngine.h"

struct MonitorInfo {
    int index = 0;
    std::wstring name;
    RECT rect{};
    bool isPrimary = false;
    UINT width = 0;
    UINT height = 0;
};

std::vector<MonitorInfo> EnumerateMonitors();

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    bool Start(int monitorIndex, UINT targetFps = 60);
    void Stop();

    bool TryGetLatestFrame(CapturedFrame& out);
    bool IsRunning() const { return m_running; }

    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }

private:
    void CaptureThreadProc(RECT monitorRect, UINT targetFps);

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    std::mutex m_frameMutex;
    CapturedFrame m_latestFrame;
    std::atomic<bool> m_hasNewFrame{false};

    UINT m_width = 0;
    UINT m_height = 0;
};
