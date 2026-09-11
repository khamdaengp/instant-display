#include "D3DRenderer.h"
#include <d3dcompiler.h>
#include <stdexcept>

namespace {

// Full-screen textured quad, positions in NDC. Aspect-ratio letterboxing is
// done by adjusting these positions per-frame (see UpdateVertexBufferForAspect)
// rather than by scaling the texture on the CPU.
struct Vertex { float x, y, u, v; };

const char* kVertexShaderSrc = R"(
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(VSIn input) {
    VSOut o;
    o.pos = float4(input.pos, 0.0, 1.0);
    o.uv = input.uv;
    return o;
}
)";

// NV12: Y plane (full res, R8) + interleaved UV plane (half res, R8G8).
// BT.601 limited-range conversion (typical for USB/UVC capture devices).
const char* kPixelShaderNV12Src = R"(
Texture2D texY : register(t0);
Texture2D texUV : register(t1);
SamplerState samp : register(s0);

float3 YuvToRgb(float y, float u, float v) {
    y = (y - 16.0/255.0) * (255.0/219.0);
    u = (u - 128.0/255.0) * (255.0/224.0);
    v = (v - 128.0/255.0) * (255.0/224.0);
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    return saturate(float3(r, g, b));
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float yv = texY.Sample(samp, uv).r;
    float2 uvv = texUV.Sample(samp, uv).rg;
    return float4(YuvToRgb(yv, uvv.x, uvv.y), 1.0);
}
)";

// YUY2: packed 4:2:2, two pixels per 4 bytes (Y0 U Y1 V). We upload it as an
// R8G8B8A8 texture where each texel holds one YUY2 macropixel, and derive
// which sub-pixel (even/odd) we're on from screen-space X in the shader.
const char* kPixelShaderYUY2Src = R"(
Texture2D texPacked : register(t0);
SamplerState samp : register(s0);

float3 YuvToRgb(float y, float u, float v) {
    y = (y - 16.0/255.0) * (255.0/219.0);
    u = (u - 128.0/255.0) * (255.0/224.0);
    v = (v - 128.0/255.0) * (255.0/224.0);
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    return saturate(float3(r, g, b));
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 texel = texPacked.Sample(samp, uv); // texel = (Y0,U,Y1,V) as RGBA8
    // pos.x is the destination pixel's screen coordinate; use its fractional
    // parity against the source macropixel to pick Y0 vs Y1.
    bool evenPixel = (fmod(floor(pos.x), 2.0) < 1.0);
    float y = evenPixel ? texel.r : texel.b;
    float u = texel.g;
    float v = texel.a;
    return float4(YuvToRgb(y, u, v), 1.0);
}
)";

ComPtr<ID3DBlob> CompileShader(const char* src, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, errBlob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                             entry, target, flags, 0, &blob, &errBlob);
    if (FAILED(hr)) {
        std::string msg = errBlob ? (const char*)errBlob->GetBufferPointer() : "unknown shader error";
        throw std::runtime_error("Shader compile failed: " + msg);
    }
    return blob;
}

} // namespace

HRESULT D3DRenderer::Init(HWND hwnd) {
    m_hwnd = hwnd;
    HRESULT hr = CreateDeviceAndSwapChain(hwnd);
    if (FAILED(hr)) return hr;
    hr = CreateShaders();
    if (FAILED(hr)) return hr;

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr = m_device->CreateSamplerState(&sampDesc, &m_sampler);
    return hr;
}

HRESULT D3DRenderer::CreateDeviceAndSwapChain(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    m_windowWidth = rc.right - rc.left;
    m_windowHeight = rc.bottom - rc.top;

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                    nullptr, 0, D3D11_SDK_VERSION,
                                    &m_device, &fl, &m_context);
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIDevice> dxgiDevice;
    m_device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = m_windowWidth;
    scd.Height = m_windowHeight;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // 2 buffers is enough for flip-model; more buffers = more possible
    // queued-but-unpresented frames = more latency.
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &scd, nullptr, nullptr, &m_swapChain);
    if (FAILED(hr)) return hr;

    // Cap the frame-latency queue at 1: DXGI will block Present() rather
    // than let more than one frame back up waiting to be displayed.
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(m_swapChain.As(&sc2))) {
        sc2->SetMaximumFrameLatency(1);
    }

    ComPtr<ID3D11Texture2D> backbuffer;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    m_device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_rtv);

    return S_OK;
}

HRESULT D3DRenderer::CreateShaders() {
    auto vsBlob = CompileShader(kVertexShaderSrc, "main", "vs_4_0");
    auto psNV12Blob = CompileShader(kPixelShaderNV12Src, "main", "ps_4_0");
    auto psYUY2Blob = CompileShader(kPixelShaderYUY2Src, "main", "ps_4_0");

    HRESULT hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                               nullptr, &m_vsPassthrough);
    if (FAILED(hr)) return hr;
    hr = m_device->CreatePixelShader(psNV12Blob->GetBufferPointer(), psNV12Blob->GetBufferSize(),
                                      nullptr, &m_psNV12);
    if (FAILED(hr)) return hr;
    hr = m_device->CreatePixelShader(psYUY2Blob->GetBufferPointer(), psYUY2Blob->GetBufferSize(),
                                      nullptr, &m_psYUY2);
    if (FAILED(hr)) return hr;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      &m_inputLayout);
    if (FAILED(hr)) return hr;

    Vertex quad[6] = {
        {-1, -1, 0, 1}, {-1, 1, 0, 0}, {1, -1, 1, 1},
        {1, -1, 1, 1}, {-1, 1, 0, 0}, {1, 1, 1, 0},
    };
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.ByteWidth = sizeof(quad);
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA init{quad, 0, 0};
    return m_device->CreateBuffer(&vbDesc, &init, &m_vertexBuffer);
}

void D3DRenderer::UpdateVertexBufferForAspect(UINT frameW, UINT frameH) {
    float x0 = -1, x1 = 1, y0 = -1, y1 = 1;
    if (m_keepAspect && frameW && frameH && m_windowWidth && m_windowHeight) {
        float srcAspect = (float)frameW / frameH;
        float dstAspect = (float)m_windowWidth / m_windowHeight;
        if (srcAspect > dstAspect) {
            float scale = dstAspect / srcAspect;
            y0 = -scale; y1 = scale;
        } else {
            float scale = srcAspect / dstAspect;
            x0 = -scale; x1 = scale;
        }
    }
    Vertex quad[6] = {
        {x0, y0, 0, 1}, {x0, y1, 0, 0}, {x1, y0, 1, 1},
        {x1, y0, 1, 1}, {x0, y1, 0, 0}, {x1, y1, 1, 0},
    };
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, quad, sizeof(quad));
        m_context->Unmap(m_vertexBuffer.Get(), 0);
    }
}

void D3DRenderer::Resize(UINT width, UINT height) {
    if (!m_swapChain || width == 0 || height == 0) return;
    m_rtv.Reset();
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ComPtr<ID3D11Texture2D> backbuffer;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    m_device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_rtv);
    m_windowWidth = width;
    m_windowHeight = height;
}

HRESULT D3DRenderer::EnsureTextures(UINT width, UINT height, PixelFormat format) {
    if (width == m_texWidth && height == m_texHeight && format == m_texFormat &&
        (m_texY || m_texPacked)) {
        return S_OK; // already sized correctly
    }
    m_texY.Reset(); m_texUV.Reset(); m_texPacked.Reset();
    m_srvY.Reset(); m_srvUV.Reset(); m_srvPacked.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.SampleDesc.Count = 1;
    desc.ArraySize = 1;
    desc.MipLevels = 1;

    HRESULT hr = S_OK;
    if (format == PixelFormat::NV12) {
        desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_R8_UNORM;
        hr = m_device->CreateTexture2D(&desc, nullptr, &m_texY);
        if (FAILED(hr)) return hr;
        m_device->CreateShaderResourceView(m_texY.Get(), nullptr, &m_srvY);

        desc.Width = width / 2; desc.Height = height / 2; desc.Format = DXGI_FORMAT_R8G8_UNORM;
        hr = m_device->CreateTexture2D(&desc, nullptr, &m_texUV);
        if (FAILED(hr)) return hr;
        m_device->CreateShaderResourceView(m_texUV.Get(), nullptr, &m_srvUV);
    } else if (format == PixelFormat::YUY2) {
        // Each YUY2 macropixel (4 bytes, 2 screen pixels) becomes one RGBA8 texel.
        desc.Width = width / 2; desc.Height = height; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = m_device->CreateTexture2D(&desc, nullptr, &m_texPacked);
        if (FAILED(hr)) return hr;
        m_device->CreateShaderResourceView(m_texPacked.Get(), nullptr, &m_srvPacked);
    } else {
        return E_NOTIMPL; // MJPEG frames should already be decoded to NV12/YUY2 upstream
    }

    m_texWidth = width; m_texHeight = height; m_texFormat = format;
    return S_OK;
}

void D3DRenderer::RenderFrame(const CapturedFrame& frame, UINT frameWidth, UINT frameHeight,
                               bool keepAspectRatio) {
    if (!m_device || frame.data.empty() || frameWidth == 0 || frameHeight == 0) return;
    m_keepAspect = keepAspectRatio;

    if (FAILED(EnsureTextures(frameWidth, frameHeight, frame.format))) return;

    // Upload: map + memcpy straight into the GPU texture. No intermediate
    // CPU-side conversion buffer.
    if (frame.format == PixelFormat::NV12) {
        UINT ySize = frameWidth * frameHeight;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_texY.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const uint8_t* src = frame.data.data();
            uint8_t* dst = (uint8_t*)mapped.pData;
            for (UINT row = 0; row < frameHeight; ++row)
                memcpy(dst + row * mapped.RowPitch, src + row * frameWidth, frameWidth);
            m_context->Unmap(m_texY.Get(), 0);
        }
        if (SUCCEEDED(m_context->Map(m_texUV.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const uint8_t* src = frame.data.data() + ySize;
            uint8_t* dst = (uint8_t*)mapped.pData;
            UINT uvW = frameWidth, uvH = frameHeight / 2; // interleaved UV row bytes == Y row bytes
            for (UINT row = 0; row < uvH; ++row)
                memcpy(dst + row * mapped.RowPitch, src + row * uvW, uvW);
            m_context->Unmap(m_texUV.Get(), 0);
        }
    } else if (frame.format == PixelFormat::YUY2) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        UINT rowBytes = frameWidth * 2; // 2 bytes/pixel packed
        if (SUCCEEDED(m_context->Map(m_texPacked.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const uint8_t* src = frame.data.data();
            uint8_t* dst = (uint8_t*)mapped.pData;
            for (UINT row = 0; row < frameHeight; ++row)
                memcpy(dst + row * mapped.RowPitch, src + row * rowBytes, rowBytes);
            m_context->Unmap(m_texPacked.Get(), 0);
        }
    } else {
        return;
    }

    UpdateVertexBufferForAspect(frameWidth, frameHeight);

    D3D11_VIEWPORT vp{0, 0, (float)m_windowWidth, (float)m_windowHeight, 0, 1};
    m_context->RSSetViewports(1, &vp);
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    float clear[4] = {0, 0, 0, 1};
    m_context->ClearRenderTargetView(m_rtv.Get(), clear);

    UINT stride = sizeof(Vertex), offset = 0;
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vsPassthrough.Get(), nullptr, 0);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    if (frame.format == PixelFormat::NV12) {
        ID3D11ShaderResourceView* srvs[2] = {m_srvY.Get(), m_srvUV.Get()};
        m_context->PSSetShaderResources(0, 2, srvs);
        m_context->PSSetShader(m_psNV12.Get(), nullptr, 0);
    } else {
        ID3D11ShaderResourceView* srvs[1] = {m_srvPacked.Get()};
        m_context->PSSetShaderResources(0, 1, srvs);
        m_context->PSSetShader(m_psYUY2.Get(), nullptr, 0);
    }

    m_context->Draw(6, 0);

    // DO_NOT_WAIT: never block the render thread on a slow present; if the
    // swapchain isn't ready we simply skip presenting this frame rather
    // than stall (the next captured frame will supersede it anyway).
    UINT presentFlags = kVsync ? 0 : DXGI_PRESENT_DO_NOT_WAIT;
    m_swapChain->Present(kVsync ? 1 : 0, presentFlags);
}
