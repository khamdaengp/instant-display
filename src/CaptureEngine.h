#pragma once
// CaptureEngine: the latency-critical core of this app.
//
// Design decisions and why they matter for latency:
//
// 1. ASYNC CALLBACK, NOT SYNCHRONOUS ReadSample() POLLING.
//    IMFSourceReaderCallback::OnReadSample fires as soon as the driver hands
//    MF a frame. We immediately re-issue ReadSample() so the pipeline is
//    always "primed" for the next frame with no idle gap.
//
// 2. SINGLE-SLOT "LATEST FRAME WINS" BUFFER, NOT A QUEUE.
//    If the render thread is a frame behind, we do NOT queue up frames to
//    show later - that is exactly the kind of buffering that adds visible
//    lag. Each new sample atomically replaces whatever was in the slot,
//    even if the old one was never consumed. The render thread always
//    grabs the newest available frame.
//
// 3. NO MF TRANSCODE / VIDEO PROCESSOR CONVERTERS IN THE READ PATH.
//    MF_READWRITE_DISABLE_CONVERTERS is set so the source reader will only
//    hand us native formats the device actually produces (NV12/YUY2/MJPEG),
//    rather than silently inserting a color-conversion MFT that adds a
//    processing stage (and often an internal buffer) before we see the
//    sample. We do the YUV->RGB conversion ourselves, on the GPU, in the
//    pixel shader (see D3DRenderer) - that's strictly faster and avoids an
//    extra CPU-side copy.
//
// 4. MJPEG IS DECODED BY THE SOURCE READER'S BUILT-IN MJPEG DECODER
//    (unavoidable if the device's only high-framerate mode is compressed),
//    but we prefer NV12/YUY2 whenever the device offers them at the same
//    resolution/framerate, since those need zero CPU decode.

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <functional>

using Microsoft::WRL::ComPtr;

enum class PixelFormat { NV12, YUY2, MJPEG, RGB32, UNKNOWN };

struct FormatOption {
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 fpsNumerator = 0;
    UINT32 fpsDenominator = 1;
    PixelFormat format = PixelFormat::UNKNOWN;
    DWORD streamIndex = 0;
    DWORD mediaTypeIndex = 0;
    std::wstring Describe() const;
};

// A single decoded/raw frame handed to the renderer. `data` layout depends
// on `format`: NV12/YUY2 are handed through mostly as-is (packed/planar
// bytes go straight into GPU textures); MJPEG frames arrive already
// decoded to a compatible format by the source reader's internal decoder.
struct CapturedFrame {
    std::vector<uint8_t> data;
    UINT32 width = 0;
    UINT32 height = 0;
    LONG stride = 0;
    PixelFormat format = PixelFormat::UNKNOWN;
    uint64_t captureQpc = 0; // QueryPerformanceCounter timestamp at arrival, for latency measurement
};

class CaptureEngine : public IMFSourceReaderCallback {
public:
    CaptureEngine();
    ~CaptureEngine();

    // Opens the device and enumerates all native format options across all
    // streams. Does not start streaming yet.
    HRESULT Open(const std::wstring& symbolicLink, std::vector<FormatOption>* outFormats);

    // Configures the reader to the given native format (no converters) and
    // begins async capture. Safe to call again to switch formats live.
    HRESULT StartStream(const FormatOption& fmt);

    void Stop();

    // Pulls the most recent frame if one has arrived since the last call.
    // Returns false if nothing new is available (caller should re-present
    // the last frame it already has rather than block).
    bool TryGetLatestFrame(CapturedFrame& out);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFSourceReaderCallback
    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD streamIndex, DWORD streamFlags,
                               LONGLONG timestamp, IMFSample* sample) override;
    STDMETHODIMP OnFlush(DWORD streamIndex) override;
    STDMETHODIMP OnEvent(DWORD streamIndex, IMFMediaEvent* event) override;

private:
    void RequestNextFrame();
    bool DecodeMJPEGWithWIC(const uint8_t* jpegData, DWORD jpegSize, std::vector<uint8_t>& outRgb);

    std::atomic<ULONG> m_refCount{1};
    ComPtr<IMFMediaSource> m_source;
    ComPtr<IMFSourceReader> m_reader;
    ComPtr<IWICImagingFactory> m_wicFactory;

    std::mutex m_frameMutex;
    CapturedFrame m_latestFrame;
    std::atomic<bool> m_hasNewFrame{false};
    std::atomic<bool> m_streaming{false};
    PixelFormat m_activeFormat = PixelFormat::UNKNOWN;
};
