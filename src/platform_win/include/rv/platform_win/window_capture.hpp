#pragma once

#include <cstdint>
#include <memory>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>

namespace rv::platform_win {

/// A window's picture, captured by Windows.Graphics.Capture (I10 step 6, ADR-0017 §7) into a D3D11 texture of a
/// device of the default adapter (the GPU the OpenGL context runs on), which OpenGL reads through WGL_NV_DX_interop2
/// (gl::DxInteropTexture). The capture shows no yellow border and no cursor where the system allows it (Windows 11).
/// A minimized window captures nothing; one in exclusive fullscreen cannot be captured (an emulator plays borderless).
/// Main thread; the captured frames arrive on a thread of the system's, poll() takes the newest.
class WindowCapture {
public:
    /// Whether this system can capture windows (GraphicsCaptureSession::IsSupported: Windows 10 1903 and later).
    [[nodiscard]] static bool supported() noexcept;
    /// Starts capturing `window` (an HWND). Errors: Unsupported (no capture on this system, a window that is gone),
    /// PlatformError (the device, the capture item or the session could not be made).
    [[nodiscard]] static Expected<std::unique_ptr<WindowCapture>> start(std::uintptr_t window);
    ~WindowCapture();
    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    /// Copies the newest frame captured since the last call into texture(): true when one came. Errors: the frame
    /// could not be read or copied.
    [[nodiscard]] Expected<bool> poll();

    /// The ID3D11Device and the ID3D11Texture2D (B8G8R8A8, the window's content size) the frames are copied into, for
    /// the interop; null before the first frame.
    [[nodiscard]] void* device() const noexcept;
    [[nodiscard]] void* texture() const noexcept;
    [[nodiscard]] glm::ivec2 size() const noexcept;
    /// Counts the times texture() was made again (the window's size changed): the interop registers it again.
    [[nodiscard]] std::uint64_t textureGeneration() const noexcept;
    /// The window closed (the capture item's Closed event).
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] std::uint64_t frames() const noexcept;

private:
    struct Impl;
    explicit WindowCapture(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

/// The window's client area in its captured picture (I10 step 11): the frame and the title bar of a windowed emulator
/// left out. Pixels of a capture of `captureSize` (the window's extended frame bounds, as Windows.Graphics.Capture
/// takes it): corner (x, y) and size (z, w). The whole capture when the window has no frame, is gone, or its areas
/// cannot be read.
[[nodiscard]] glm::ivec4 clientAreaInCapture(std::uintptr_t window, glm::ivec2 captureSize) noexcept;

} // namespace rv::platform_win
