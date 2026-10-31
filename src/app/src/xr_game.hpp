#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include <rv/app/picture_bounds.hpp>
#include <rv/app/run_report.hpp>
#include <rv/platform_win/window_capture.hpp>
#include <rv/render/dx_interop.hpp>
#include <rv/render/picture_probe.hpp>

namespace rv::gl {
class RenderStateCache;
} // namespace rv::gl

namespace rv::app {

class ScreenService;

/// The launched game on the room's CRT in VR (I10 step 6, ADR-0017 §7): while the launch's emulator shows a window,
/// its picture is captured (platform_win::WindowCapture: Windows.Graphics.Capture into D3D11) and read by OpenGL each
/// frame a new one came (gl::DxInteropTexture), and the hero CRT shows it in place of its clips
/// (ScreenService::showGameWindow). The capture ends with the window (the game's end), and the CRT has its pictures
/// back. A capture that cannot start or breaks is logged and recorded once per window, and the game plays on without
/// it. Retroverse never touches the emulator: it only reads its window. Main thread, GL context current.
class XrGame {
public:
    XrGame() = default;
    ~XrGame();
    XrGame(const XrGame&) = delete;
    XrGame& operator=(const XrGame&) = delete;

    /// One frame with the launch's emulator `window` (an HWND; 0: none, the capture ends).
    void update(std::uintptr_t window, gl::RenderStateCache& state, ScreenService* screen, XrRecord* record);
    /// The CRT shows the game.
    [[nodiscard]] bool showing() const noexcept { return m_showing; }

private:
    void stop(ScreenService* screen);
    /// I10 step 11: a small copy of the window read a few times a second, its black bars cropped off the CRT.
    void probeBars(gl::RenderStateCache& state, XrRecord* record);
    void fail(std::string_view what, const Error& error, ScreenService* screen, XrRecord* record);

    std::uintptr_t m_window = 0;
    /// The window whose capture failed: not tried again.
    std::uintptr_t m_failed = 0;
    std::unique_ptr<platform_win::WindowCapture> m_capture;
    std::unique_ptr<gl::DxInteropTexture> m_interop;
    std::uint64_t m_generation = 0;
    bool m_showing = false;
    std::unique_ptr<gl::PictureProbe> m_probe;
    bool m_probeFailed = false;
    std::chrono::steady_clock::time_point m_probedAt{};
    PictureBounds m_bounds;
    /// The window's client area in the capture's pixels (its frame and title bar left out), and the crop the CRT shows:
    /// the client area's part between its bars, in the capture's texture coordinates.
    glm::ivec4 m_client{0};
    glm::vec4 m_crop{0.0f, 0.0f, 1.0f, 1.0f};
};

} // namespace rv::app
