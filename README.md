# InstantDisplay

A minimal, ultra-low-latency Windows desktop app that reads a UVC/DirectShow capture card via
**Media Foundation** and renders it with **Direct3D 11**, built specifically
to minimize the delay between "frame leaves the capture card" and "frame is
on screen."

## Why this stack (not DirectShow, not Python/OpenCV)

- **Media Foundation**, async `IMFSourceReaderCallback`, is Microsoft's
  current-generation capture API. DirectShow's filter graph adds its own
  buffering/queuing between filters that's hard to fully disable.
- **Python + OpenCV's `VideoCapture`** wraps DirectShow or MSMF internally
  and does its own frame buffering you can't fully turn off, plus GIL/frame-
  pacing overhead. Not used here because latency is the stated priority.
- **Direct3D11 with flip-model present** (`DXGI_SWAP_EFFECT_FLIP_DISCARD`)
  avoids the DWM legacy blit copy and lets us cap the present queue at a
  single frame (`SetMaximumFrameLatency(1)`).

## Where the "zero-buffer" design lives in the code

| Concern | File | Mechanism |
|---|---|---|
| No queued capture samples | `CaptureEngine.cpp` | single-slot "latest frame wins" buffer, overwritten in place; async re-arm on every callback |
| No MF converter/resizer MFTs | `CaptureEngine.cpp` | `MF_READWRITE_DISABLE_CONVERTERS = TRUE`, native format only |
| No CPU-side color conversion pass | `D3DRenderer.cpp` | raw NV12/YUY2 bytes uploaded straight to GPU textures; YUV→RGB done in the pixel shader |
| No present-queue backlog | `D3DRenderer.cpp` | 2-buffer flip-model swapchain, `SetMaximumFrameLatency(1)`, `DXGI_PRESENT_DO_NOT_WAIT` |
| No fixed-rate render timer | `App.cpp` | render thread polls for the newest frame every ~1ms rather than waiting on a vsync-locked timer |
| Format choice exposed to the user | `App.cpp` (Format menu) | enumerates the device's *native* modes only — NV12/YUY2 preferred, MJPEG available if that's all the device offers |

## Realistic latency expectations

"Absolute zero" isn't physically achievable — there's always at least:
capture card's own internal buffering (device/driver dependent, usually
1 frame) + USB/PCIe transfer time + one GPU present. What this app removes
is every *extra* millisecond an application can control: no extra software
queueing, no unnecessary format conversion passes, no vsync-forced wait
(with vsync off, which is the default here). In practice this gets you to
roughly one frame of glass-to-glass delay, which is about as good as a
userspace Windows app gets without going through a custom kernel driver.

## Build instructions

### Prerequisites
- Windows 10 or 11
- Visual Studio 2022 (Desktop development with C++ workload) **or** the
  standalone Build Tools + CMake
- Windows 10/11 SDK (installed automatically with the above) — provides
  `mfplat.lib`, `mfreadwrite.lib`, `d3d11.lib`, etc. No external
  dependencies to download.
- CMake 3.20+

### Steps (Visual Studio / Developer Command Prompt)

```bat
git clone <this project folder as a repo, or just copy the files>
cd lowlatency-capture
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

The executable will be at `build/Release/InstantDisplay.exe`.

### Steps (CLI only, no full Visual Studio)

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd lowlatency-capture
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Running

Run the `.exe` directly (no installer needed). On launch it:
1. Enumerates connected UVC/capture-card devices into the **Device** menu
   and opens the first one automatically.
2. Enumerates that device's native formats into the **Format** menu,
   auto-selecting the highest-resolution NV12/YUY2 mode (falling back to
   MJPEG only if the device offers nothing else).

### Controls
| Action | Input |
|---|---|
| Fullscreen toggle | `F11`, or double-click the video |
| Exit fullscreen | `Esc` |
| Keep aspect ratio toggle | `A`, or View → Keep Aspect Ratio |
| VSync toggle (off = lowest latency, default) | `V`, or View → VSync |
| Switch capture device | Device menu |
| Switch resolution/format | Format menu |

## Known limitations / things to verify on real hardware

- **MJPEG path**: this build enumerates MJPEG as a selectable native format
  but the renderer currently expects NV12/YUY2 bytes. If your card's only
  high-fps mode is MJPEG, you'll need to add an MJPEG decode step (e.g. via
  a Media Foundation MJPEG decoder MFT inserted just for that path, or
  Media Foundation's `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING`)
  before the frame reaches `D3DRenderer` — noted as a TODO in
  `D3DRenderer::EnsureTextures`.
- **Audio** is intentionally not captured. If your capture card carries
  HDMI audio and you want it played back, that's a separate WASAPI
  low-latency path (shared buffer/exclusive mode) — not included here to
  keep the video path as simple and low-latency as possible.
- **Odd resolutions**: the NV12/YUY2 texture math assumes even width and
  height (true for essentially all real capture devices/cards).
- Not tested against physical hardware in this environment — Media
  Foundation and Direct3D11 are Windows-only APIs with no Linux
  equivalent, so this was written and reviewed for correctness but not
  compiled/run here. Please build and test on your target machine, and
  treat the MJPEG note above as the most likely thing to need adjustment
  for your specific card.
