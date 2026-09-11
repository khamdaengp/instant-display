#pragma once
// DeviceEnum: enumerates Media Foundation video capture sources (UVC devices,
// PCIe capture cards that expose a UVC-style driver, etc).
//
// This intentionally does NOT go through DirectShow. Modern Windows 10/11
// UVC drivers register with Media Foundation directly, and MF's async
// IMFSourceReaderCallback path gives us frame delivery without the extra
// filter-graph buffering that DirectShow's graph manager tends to add.

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct CaptureDeviceInfo {
    std::wstring friendlyName;
    std::wstring symbolicLink; // stable identifier used to re-activate the device
    bool isScreenCapture = false;
    int monitorIndex = 0;
};

// Enumerates all connected MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP devices.
// Returns an empty vector (not an error) if none are connected.
std::vector<CaptureDeviceInfo> EnumerateCaptureDevices();

// Activates (opens) a device by symbolic link and returns an IMFMediaSource.
// Returns nullptr on failure; check the HRESULT out-param for diagnostics.
ComPtr<IMFMediaSource> ActivateCaptureDevice(const std::wstring& symbolicLink, HRESULT* hrOut = nullptr);
