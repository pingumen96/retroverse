#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

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
/// back. The window is the one the launcher follows (I11 step 11: a splash then the game, a render window, a window
/// made again); a new one restarts the capture. A capture that cannot start, breaks or ends with its window closed is
/// logged and recorded, tried again a little later a few times, and the game plays on without it. A window that shows
/// only black for 5 s is reported (exclusive fullscreen, ADR-0020 §5). Retroverse never touches the emulator: it only
/// reads its window. Main thread, GL context current.
class XrGame {
public:
    XrGame() = default;
    ~XrGame();
    XrGame(const XrGame&) = delete;
    XrGame& operator=(const XrGame&) = delete;

    /// One frame with the launch's emulator `window` (an HWND; 0: none, the capture ends).
    void update(std::uintptr_t window, gl::RenderStateCache& state, ScreenService* screen, XrRecord* record);
    /// The CRT shows the game: at its last new picture a hero screen was bound and took it.
    [[nodiscard]] bool showing() const noexcept { return m_showing; }
    /// The window followed has shown only black for 5 s (ADR-0020 §5: exclusive fullscreen, I11 step 13).
    [[nodiscard]] bool black() const noexcept { return m_blackReported; }

private:
    void stop(ScreenService* screen);
    /// I10 step 11: a small copy of the window read a few times a second, its black bars cropped off the CRT.
    void probeBars(gl::RenderStateCache& state, XrRecord* record);
    void fail(std::string_view what, const Error& error, ScreenService* screen, XrRecord* record);

    std::uintptr_t m_window = 0;
    /// The window whose capture failed or closed, when, and how many times: tried again kRetryDelay later, at most
    /// kMaxAttempts times (I11 step 11: a window not yet ready to be captured, one closed and made again under the same
    /// handle).
    std::uintptr_t m_failed = 0;
    std::chrono::steady_clock::time_point m_failedAt{};
    int m_attempts = 0;
    /// Since when the window's samples are all black (nothing: the last was not); reported once per window.
    std::optional<std::chrono::steady_clock::time_point> m_blackSince;
    bool m_blackReported = false;
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
