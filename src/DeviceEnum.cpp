#include "DeviceEnum.h"
#include <mfapi.h>
#include <mfidl.h>

std::vector<CaptureDeviceInfo> EnumerateCaptureDevices() {
    std::vector<CaptureDeviceInfo> result;

    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr)) return result;

    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) return result;

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attrs.Get(), &devices, &count);
    if (FAILED(hr)) return result;

    for (UINT32 i = 0; i < count; ++i) {
        CaptureDeviceInfo info;

        WCHAR* name = nullptr;
        UINT32 nameLen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen))) {
            info.friendlyName = name;
            CoTaskMemFree(name);
        } else {
            info.friendlyName = L"Unknown device";
        }

        WCHAR* link = nullptr;
        UINT32 linkLen = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &linkLen))) {
            info.symbolicLink = link;
            CoTaskMemFree(link);
        }

        if (!info.symbolicLink.empty()) {
            result.push_back(std::move(info));
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);

    return result;
}

ComPtr<IMFMediaSource> ActivateCaptureDevice(const std::wstring& symbolicLink, HRESULT* hrOut) {
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) { if (hrOut) *hrOut = hr; return nullptr; }

    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) { if (hrOut) *hrOut = hr; return nullptr; }

    hr = attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                           symbolicLink.c_str());
    if (FAILED(hr)) { if (hrOut) *hrOut = hr; return nullptr; }

    // Re-enumerate and match, because MF requires an IMFActivate (not a raw
    // string) to instantiate the source. We rebuild the activator by
    // searching current devices for a matching symbolic link.
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attrs.Get(), &devices, &count);
    // Note: symbolic-link-scoped enumeration above already narrows this,
    // but on some drivers it returns the full list; disambiguate below.

    ComPtr<IMFMediaSource> source;
    if (SUCCEEDED(hr)) {
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* link = nullptr;
            UINT32 linkLen = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &linkLen))) {
                if (symbolicLink == link && !source) {
                    IMFMediaSource* raw = nullptr;
                    hr = devices[i]->ActivateObject(IID_PPV_ARGS(&raw));
                    if (SUCCEEDED(hr)) {
                        source.Attach(raw);
                    }
                }
                CoTaskMemFree(link);
            }
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
    }

    if (hrOut) *hrOut = source ? S_OK : E_FAIL;
    return source;
}
