#pragma once
// D3DRenderer: takes raw NV12/YUY2/MJPEG(decoded) bytes and puts them on
// screen with minimal added latency.
//
// Key choices:
// - DXGI_SWAP_EFFECT_FLIP_DISCARD (flip model): lets us skip DWM's legacy
//   blit-model copy step and, combined with a 2-buffer swapchain, keeps the
//   present queue shallow.
// - Present(0, DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_RESTART): we render
//   as soon as a frame is ready rather than waiting on a fixed vsync
//   cadence. Set kVsync=true below if screen tearing bothers you more than
//   the extra ~1 frame of latency vsync-on adds.
// - Colorspace conversion (NV12/YUY2 -> RGB) happens in a pixel shader on
//   the GPU, not on the CPU. We upload the raw planes directly into a
//   texture the shader reads from - no CPU-side color conversion pass.

#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <cstdint>
#include "CaptureEngine.h"

using Microsoft::WRL::ComPtr;

class D3DRenderer {
public:
    bool kVsync = false; // off by default: lowest latency, may tear

    HRESULT Init(HWND hwnd);
    void Resize(UINT width, UINT height);

    // Uploads one captured frame and presents it immediately. Call this
    // directly from the frame-ready path (or a tight render-thread loop
    // polling CaptureEngine::TryGetLatestFrame) - do not queue frames
    // before calling this.
    void RenderFrame(const CapturedFrame& frame, UINT frameWidth, UINT frameHeight,
                      bool keepAspectRatio);

    void SetKeepAspect(bool keep) { m_keepAspect = keep; }

private:
    HRESULT CreateDeviceAndSwapChain(HWND hwnd);
    HRESULT CreateShaders();
    HRESULT EnsureTextures(UINT width, UINT height, PixelFormat format);
    void UpdateVertexBufferForAspect(UINT frameW, UINT frameH);

    HWND m_hwnd = nullptr;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_rtv;

    ComPtr<ID3D11VertexShader> m_vsPassthrough;
    ComPtr<ID3D11PixelShader> m_psNV12;
    ComPtr<ID3D11PixelShader> m_psYUY2;
    ComPtr<ID3D11PixelShader> m_psRGB;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11SamplerState> m_sampler;

    // NV12 uses two textures (Y plane R8, UV plane R8G8); YUY2 uses one
    // packed texture sampled as R8G8B8A8-ish; RGB32 uses standard BGRA8 texture.
    ComPtr<ID3D11Texture2D> m_texY, m_texUV, m_texPacked, m_texRGB;
    ComPtr<ID3D11ShaderResourceView> m_srvY, m_srvUV, m_srvPacked, m_srvRGB;

    UINT m_texWidth = 0, m_texHeight = 0;
    PixelFormat m_texFormat = PixelFormat::UNKNOWN;

    UINT m_windowWidth = 0, m_windowHeight = 0;
    bool m_keepAspect = true;
    std::mutex m_renderMutex;
};
