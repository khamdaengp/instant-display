# InstantDisplay (v1.1.0)

A lightweight, ultra-low-latency Windows desktop application for HDMI capture cards, webcams, and real-time desktop monitor capture. Built with **Windows Media Foundation** and **Direct3D 11** to minimize the delay between "frame arrives at the capture device" and "frame is displayed on screen."

---

## 🚀 Download Pre-built Release (No installation required)

You can download and run the standalone portable executable immediately:

| Version | File | Direct Download |
|---|---|---|
| **v1.1.0 (Latest)** | `InstantDisplay 1.1.0.exe` | [⬇️ Download InstantDisplay 1.1.0.exe](https://github.com/khamdaengp/instant-display/raw/main/Release/InstantDisplay%201.1.0.exe) |
| **v1.1.0 (Standard)** | `InstantDisplay.exe` | [⬇️ Download InstantDisplay.exe](https://github.com/khamdaengp/instant-display/raw/main/Release/InstantDisplay.exe) |

*Also available directly in the [`Release/`](./Release) folder of this repository.*

---

## ⚡ Key Features

- **Ultra-Low Latency Direct-to-GPU Pipeline:** Direct3D 11 flip-model presentation (`DXGI_SWAP_EFFECT_FLIP_DISCARD`) capped at a single frame latency (`SetMaximumFrameLatency(1)`).
- **Zero-Buffer Architecture:** Single-slot "latest frame wins" mechanism with asynchronous re-arming. No frame queuing, no internal software delays.
- **MJPEG 1080p @ 60fps Support:** Clean Media Foundation hardware MFT decoding and fail-safe WIC (Windows Imaging Component) decoding for budget USB 2.0 / USB 3.0 HDMI capture cards (e.g. Cam Link clones, MacroSilicon MS2109).
- **Desktop Monitor Capture:** Real-time desktop capture with hardware mouse cursor rendering at smooth 60fps.
- **Native Raw YUV Formats:** Direct GPU pixel shader color conversion for NV12 and YUY2 with 0ms CPU overhead.
- **Aspect Ratio & VSync Control:** Toggle aspect-ratio preservation and VSync on the fly.
- **Fullscreen Mode:** One-click or hotkey fullscreen toggle with zero window borders.

---

## 🎮 Controls & Shortcuts

| Action | Input |
|---|---|
| **Toggle Fullscreen** | `F11` or **Double-Click** anywhere on video |
| **Exit Fullscreen** | `Esc` |
| **Keep Aspect Ratio** | `A` or menu `View -> Keep Aspect Ratio` |
| **Toggle VSync** | `V` or menu `View -> VSync` *(VSync OFF = lowest latency)* |
| **Mirror / Flip Horizontal** | `M` or menu `Color -> Mirror Horizontal` |
| **Toggle Color Range** | `C` or menu `Color -> Limited Range / Full Range` |
| **Adjust Brightness** | `Ctrl + Up` (+5%) / `Ctrl + Down` (-5%) |
| **Adjust Contrast** | `Ctrl + Right` (+10%) / `Ctrl + Left` (-10%) |
| **Adjust Saturation** | `Alt + Right` (+10%) / `Alt + Left` (-10%) |
| **Reset Colors** | `R` or menu `Color -> Reset Colors to Default` |
| **Select Capture Device** | `Device` menu |
| **Select Resolution / FPS / Format** | `Format` menu |
| **About & Info** | `Help -> About InstantDisplay v1.1.0` |

---

## 🛠️ Building from Source

### Prerequisites
- **Windows 10 or 11**
- **Visual Studio 2022** (Desktop development with C++) or Visual Studio Build Tools
- **Windows 10/11 SDK** (standard with Visual Studio)
- **CMake 3.20+**

### Build Commands (PowerShell / Command Prompt)

```powershell
# Clone the repository
git clone https://github.com/khamdaengp/instant-display.git
cd instant-display

# Configure and build Release binary
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output executables will be generated in `build\Release\`:
- `build\Release\InstantDisplay 1.1.0.exe`
- `build\Release\InstantDisplay.exe`

---

## 🏗️ Architecture Overview

| Concern | File | Mechanism |
|---|---|---|
| **No queued capture samples** | `src/CaptureEngine.cpp` | Single-slot "latest frame wins" buffer; immediate async re-arm on every sample callback. |
| **Fast format switching** | `src/CaptureEngine.cpp` | Clean reader recreation & source activation preventing `MF_E_SHUTDOWN` and `MF_E_INVALIDREQUEST`. |
| **MJPEG decoding** | `src/CaptureEngine.cpp` | Dual-layer: Media Foundation Decoder MFT + fallback Windows Imaging Component (WIC) decode. |
| **No CPU color conversion** | `src/D3DRenderer.cpp` | Raw NV12/YUY2 uploaded straight to GPU textures; YUV→RGB conversion done in pixel shaders. |
| **Minimum present lag** | `src/D3DRenderer.cpp` | 2-buffer flip-model swapchain, `SetMaximumFrameLatency(1)`. |
| **Desktop Capture** | `src/ScreenCapture.cpp` | Real-time desktop capture with hardware cursor overlay. |

---

## 📄 License
MIT License. Free for personal and commercial use.
