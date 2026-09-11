#include "CaptureEngine.h"
#include <mferror.h>
#include <windows.h>
#include <sstream>

namespace {

PixelFormat SubtypeToFormat(const GUID& g) {
    if (g == MFVideoFormat_NV12) return PixelFormat::NV12;
    if (g == MFVideoFormat_YUY2) return PixelFormat::YUY2;
    if (g == MFVideoFormat_MJPG) return PixelFormat::MJPEG;
    if (g == MFVideoFormat_RGB32 || g == MFVideoFormat_ARGB32) return PixelFormat::RGB32;
    return PixelFormat::UNKNOWN;
}

const wchar_t* FormatName(PixelFormat f) {
    switch (f) {
        case PixelFormat::NV12: return L"NV12";
        case PixelFormat::YUY2: return L"YUY2";
        case PixelFormat::MJPEG: return L"MJPEG";
        case PixelFormat::RGB32: return L"RGB32";
        default: return L"UNKNOWN";
    }
}

} // namespace

std::wstring FormatOption::Describe() const {
    std::wstringstream ss;
    double fps = fpsDenominator ? (double)fpsNumerator / fpsDenominator : 0.0;
    ss << width << L"x" << height << L" @ " << fps << L"fps [" << FormatName(format) << L"]";
    return ss.str();
}

CaptureEngine::CaptureEngine() {}
CaptureEngine::~CaptureEngine() { Stop(); }

HRESULT CaptureEngine::CreateSource(ComPtr<IMFMediaSource>& outSource) {
    if (m_symbolicLink.empty()) return E_FAIL;

    ComPtr<IMFAttributes> devAttrs;
    HRESULT hr = MFCreateAttributes(&devAttrs, 2);
    if (FAILED(hr)) return hr;
    devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    devAttrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, m_symbolicLink.c_str());

    IMFActivate** activators = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(devAttrs.Get(), &activators, &count);
    if (FAILED(hr) || count == 0) return FAILED(hr) ? hr : E_FAIL;

    IMFMediaSource* src = nullptr;
    for (UINT32 i = 0; i < count; ++i) {
        if (!src) {
            WCHAR* link = nullptr; UINT32 len = 0;
            if (SUCCEEDED(activators[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len))) {
                if (m_symbolicLink == link) {
                    activators[i]->ActivateObject(IID_PPV_ARGS(&src));
                }
                CoTaskMemFree(link);
            }
        }
        activators[i]->Release();
    }
    CoTaskMemFree(activators);
    if (!src) return E_FAIL;
    outSource.Attach(src);
    return S_OK;
}

HRESULT CaptureEngine::Open(const std::wstring& symbolicLink, std::vector<FormatOption>* outFormats) {
    m_symbolicLink = symbolicLink;

    HRESULT hr = CreateSource(m_source);
    if (FAILED(hr)) return hr;

    // Reader attributes: async callback mode + explicitly refuse converter
    // MFTs so we only ever see the device's native output formats during enum.
    ComPtr<IMFAttributes> readerAttrs;
    hr = MFCreateAttributes(&readerAttrs, 4);
    if (FAILED(hr)) return hr;
    readerAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback*>(this));
    readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    readerAttrs->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
    readerAttrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);

    hr = MFCreateSourceReaderFromMediaSource(m_source.Get(), readerAttrs.Get(), &m_reader);
    if (FAILED(hr)) return hr;

    // Enumerate every native media type on stream 0 (typical single video
    // stream capture card / webcam layout).
    if (outFormats) {
        outFormats->clear();
        DWORD streamIndex = 0;
        for (DWORD typeIndex = 0; ; ++typeIndex) {
            ComPtr<IMFMediaType> mt;
            hr = m_reader->GetNativeMediaType(streamIndex, typeIndex, &mt);
            if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;

            GUID subtype{};
            mt->GetGUID(MF_MT_SUBTYPE, &subtype);
            PixelFormat fmt = SubtypeToFormat(subtype);
            if (fmt == PixelFormat::UNKNOWN) continue; // skip formats we don't render

            UINT32 w = 0, h = 0;
            MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
            UINT32 num = 0, den = 1;
            MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &num, &den);

            FormatOption opt;
            opt.width = w; opt.height = h;
            opt.fpsNumerator = num; opt.fpsDenominator = den;
            opt.format = fmt;
            opt.streamIndex = streamIndex;
            opt.mediaTypeIndex = typeIndex;
            outFormats->push_back(opt);
        }
    }

    return S_OK;
}

HRESULT CaptureEngine::StartStream(const FormatOption& fmt) {
    m_streaming = false;

    // 1. Flush and release any active reader so that no pending async reads interfere
    if (m_reader) {
        m_reader->Flush(MF_SOURCE_READER_ALL_STREAMS);
        m_reader.Reset();
    }

    // 2. Shut down previous source cleanly and re-create a clean media source
    if (m_source) {
        m_source->Shutdown();
        m_source.Reset();
    }
    HRESULT hr = CreateSource(m_source);
    if (FAILED(hr)) return hr;

    // 3. Re-create reader with appropriate attributes for the requested format
    ComPtr<IMFAttributes> readerAttrs;
    hr = MFCreateAttributes(&readerAttrs, 5);
    if (FAILED(hr)) return hr;

    readerAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback*>(this));
    readerAttrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    readerAttrs->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);

    if (fmt.format == PixelFormat::MJPEG) {
        // Enable video processing and converters so Media Foundation can automatically
        // decode MJPEG to RGB32/NV12 using its built-in MJPEG MFT decoder.
        readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    } else {
        // Direct zero-latency pipeline for native uncompressed raw formats (NV12 / YUY2)
        readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    }

    hr = MFCreateSourceReaderFromMediaSource(m_source.Get(), readerAttrs.Get(), &m_reader);
    if (FAILED(hr)) return hr;

    // 4. Set the capture media type
    if (fmt.format == PixelFormat::MJPEG) {
        // Set native type first so hardware camera/capture card runs at selected resolution & fps
        ComPtr<IMFMediaType> nativeType;
        hr = m_reader->GetNativeMediaType(fmt.streamIndex, fmt.mediaTypeIndex, &nativeType);
        if (FAILED(hr)) return hr;
        hr = m_reader->SetCurrentMediaType(fmt.streamIndex, nullptr, nativeType.Get());
        if (FAILED(hr)) return hr;

        // Request decoded RGB32 output from the reader
        ComPtr<IMFMediaType> decodeType;
        hr = MFCreateMediaType(&decodeType);
        if (SUCCEEDED(hr)) {
            decodeType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            decodeType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            MFSetAttributeSize(decodeType.Get(), MF_MT_FRAME_SIZE, fmt.width, fmt.height);
            MFSetAttributeRatio(decodeType.Get(), MF_MT_FRAME_RATE, fmt.fpsNumerator, fmt.fpsDenominator);

            HRESULT hrDecode = m_reader->SetCurrentMediaType(fmt.streamIndex, nullptr, decodeType.Get());
            if (FAILED(hrDecode)) {
                // Fallback 1: try NV12
                decodeType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                hrDecode = m_reader->SetCurrentMediaType(fmt.streamIndex, nullptr, decodeType.Get());
            }
            if (FAILED(hrDecode)) {
                // Fallback 2: try YUY2
                decodeType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2);
                hrDecode = m_reader->SetCurrentMediaType(fmt.streamIndex, nullptr, decodeType.Get());
            }
            // If all decoders fail to set on the reader, the reader will output native MJPEG,
            // which will be seamlessly decoded by our fallback WIC decoder in OnReadSample.
        }
    } else {
        ComPtr<IMFMediaType> mt;
        hr = m_reader->GetNativeMediaType(fmt.streamIndex, fmt.mediaTypeIndex, &mt);
        if (FAILED(hr)) return hr;

        hr = m_reader->SetCurrentMediaType(fmt.streamIndex, nullptr, mt.Get());
        if (FAILED(hr)) return hr;
    }

    // 4. Update m_activeFormat to the actual format the reader will output
    ComPtr<IMFMediaType> currentType;
    if (SUCCEEDED(m_reader->GetCurrentMediaType(fmt.streamIndex, &currentType))) {
        GUID subtype{};
        currentType->GetGUID(MF_MT_SUBTYPE, &subtype);
        m_activeFormat = SubtypeToFormat(subtype);
    } else {
        m_activeFormat = fmt.format;
    }

    // 5. Select only the active stream
    DWORD si = 0;
    ComPtr<IMFMediaType> probe;
    while (SUCCEEDED(m_reader->GetNativeMediaType(si, 0, &probe))) {
        m_reader->SetStreamSelection(si, FALSE);
        probe.Reset();
        ++si;
    }
    m_reader->SetStreamSelection(fmt.streamIndex, TRUE);

    m_streaming = true;
    RequestNextFrame();
    return S_OK;
}

void CaptureEngine::Stop() {
    m_streaming = false;
    if (m_reader) {
        m_reader->Flush(MF_SOURCE_READER_ALL_STREAMS);
    }
}

void CaptureEngine::RequestNextFrame() {
    if (!m_streaming || !m_reader) return;
    // Async ReadSample: returns immediately; OnReadSample fires later on an
    // MF work-queue thread. We re-arm here and again inside the callback so
    // there is never a gap where the reader is idle waiting to be asked.
    m_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
}

bool CaptureEngine::TryGetLatestFrame(CapturedFrame& out) {
    if (!m_hasNewFrame.exchange(false)) return false;
    std::lock_guard<std::mutex> lock(m_frameMutex);
    out = m_latestFrame; // copy out; keep the slot for the next overwrite
    return true;
}

bool CaptureEngine::DecodeMJPEGWithWIC(const uint8_t* jpegData, DWORD jpegSize, std::vector<uint8_t>& outRgb) {
    if (!m_wicFactory) {
        HRESULT hrCo = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&m_wicFactory));
        if (FAILED(hrCo)) return false;
    }

    ComPtr<IWICStream> stream;
    HRESULT hr = m_wicFactory->CreateStream(&stream);
    if (FAILED(hr)) return false;

    hr = stream->InitializeFromMemory(const_cast<BYTE*>(jpegData), jpegSize);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = m_wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return false;

    UINT width = 0, height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) return false;

    ComPtr<IWICFormatConverter> converter;
    hr = m_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return false;

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    UINT stride = width * 4;
    UINT bufferSize = stride * height;
    outRgb.resize(bufferSize);
    hr = converter->CopyPixels(nullptr, stride, bufferSize, outRgb.data());
    return SUCCEEDED(hr);
}

STDMETHODIMP CaptureEngine::OnReadSample(HRESULT hrStatus, DWORD streamIndex, DWORD streamFlags,
                                          LONGLONG timestamp, IMFSample* sample) {
    (void)streamIndex; (void)timestamp;

    if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
        return S_OK;
    }

    if (SUCCEEDED(hrStatus) && sample) {
        ComPtr<IMFMediaBuffer> buffer;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (SUCCEEDED(buffer->Lock(&data, &maxLen, &curLen))) {
                LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);

                // Fallback: If the incoming frame is raw compressed JPEG (starts with 0xFF 0xD8), decode via WIC
                if (m_activeFormat == PixelFormat::MJPEG || (curLen >= 2 && data[0] == 0xFF && data[1] == 0xD8)) {
                    std::vector<uint8_t> rgbData;
                    if (DecodeMJPEGWithWIC(data, curLen, rgbData)) {
                        std::lock_guard<std::mutex> lock(m_frameMutex);
                        m_latestFrame.data = std::move(rgbData);
                        m_latestFrame.format = PixelFormat::RGB32;
                        m_latestFrame.captureQpc = (uint64_t)qpc.QuadPart;
                        buffer->Unlock();
                        m_hasNewFrame = true;
                        RequestNextFrame();
                        return S_OK;
                    }
                }

                std::lock_guard<std::mutex> lock(m_frameMutex);
                // Overwrite in place; this is the "latest frame wins" slot.
                m_latestFrame.data.assign(data, data + curLen);
                m_latestFrame.format = m_activeFormat;
                m_latestFrame.captureQpc = (uint64_t)qpc.QuadPart;
                // Width/height/stride are filled by the renderer from the
                // active FormatOption (kept outside this struct here for
                // simplicity); see App.cpp where frames are consumed.
                buffer->Unlock();
                m_hasNewFrame = true;
            }
        }
    }

    // Immediately re-arm for the next sample. This is the crux of the
    // "zero-buffer" pipeline: we only ever have one outstanding read and
    // one held frame, never a queue of pending samples.
    RequestNextFrame();
    return S_OK;
}

STDMETHODIMP CaptureEngine::OnFlush(DWORD) { return S_OK; }
STDMETHODIMP CaptureEngine::OnEvent(DWORD, IMFMediaEvent*) { return S_OK; }

STDMETHODIMP CaptureEngine::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFSourceReaderCallback)) {
        *ppv = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CaptureEngine::AddRef() { return ++m_refCount; }
STDMETHODIMP_(ULONG) CaptureEngine::Release() {
    ULONG c = --m_refCount;
    // Lifetime owned by CaptureEngine's caller (App), not by MF ref
    // counting alone - avoid self-delete here since this is a stack/App-
    // owned member, not a heap-allocated COM object.
    return c;
}
