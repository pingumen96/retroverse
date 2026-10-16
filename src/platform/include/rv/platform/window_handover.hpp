#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;

namespace rv::platform {

/// What Retroverse's window does while an emulator runs (the launch spike compares the two; ADR-0006).
enum class HandoverPolicy : std::uint8_t {
    Minimize, ///< minimized while the emulator runs, restored afterwards
    Keep,     ///< left where it is, without rendering, and raised afterwards
};

/// "minimize", "keep".
[[nodiscard]] std::string_view toString(HandoverPolicy policy) noexcept;
[[nodiscard]] std::optional<HandoverPolicy> parseHandoverPolicy(std::string_view text) noexcept;

/// A rectangle in physical pixels (Win32 coordinates).
struct PixelRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] int width() const noexcept { return right - left; }
    [[nodiscard]] int height() const noexcept { return bottom - top; }
    friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

/// The state of a window, as SDL and Windows see it.
struct WindowState {
    bool fullscreen = false;        ///< SDL_WINDOW_FULLSCREEN
    bool desktopFullscreen = false; ///< fullscreen without a display mode of its own (borderless, at the desktop mode)
    bool minimized = false;         ///< SDL_WINDOW_MINIMIZED or IsIconic
    bool foreground = false;        ///< GetForegroundWindow() is this window
    std::uint32_t display = 0;      ///< SDL_DisplayID
    int displayWidth = 0;           ///< the display's current mode
    int displayHeight = 0;
    float displayRefreshRate = 0.0f;
    PixelRect windowRect;           ///< GetWindowRect
    PixelRect monitorRect;          ///< the rectangle of the monitor it is on (GetMonitorInfo)
};

/// Reads the state of `window`.
[[nodiscard]] WindowState captureWindowState(SDL_Window* window);
/// True when the window is in fullscreen, not minimized, and exactly covers its monitor.
[[nodiscard]] bool coversMonitor(const WindowState& state) noexcept;
/// The HWND of an SDL window (SDL_PROP_WINDOW_WIN32_HWND_POINTER), or nullptr.
[[nodiscard]] void* nativeWindowHandle(SDL_Window* window) noexcept;

/// Ways to bring a window to the foreground when Windows' foreground lock refuses the plain request, tried one at a
/// time in this order.
enum class ForegroundRemedy : std::uint8_t {
    Raise,             ///< SDL_RaiseWindow: SetForegroundWindow
    AttachThreadInput, ///< SetForegroundWindow with this thread's input attached to the foreground window's thread
                       ///< (never to a hung thread, always detached afterwards)
    AltKey,            ///< an Alt key press injected (SendInput) before SetForegroundWindow, released after
    Flash,             ///< the last resort, a notification: the taskbar button flashes until the user comes back
};

[[nodiscard]] std::string_view toString(ForegroundRemedy remedy) noexcept;

/// One remedy tried, and whether the window was in the foreground right after it.
struct ForegroundAttempt {
    ForegroundRemedy remedy = ForegroundRemedy::Raise;
    std::chrono::steady_clock::time_point at{};
    bool foregroundAfter = false;
};

/// A change of the foreground window seen by the hand-over's event hook (EVENT_SYSTEM_FOREGROUND).
struct ForegroundChange {
    std::chrono::steady_clock::time_point at{};
    std::uint32_t processId = 0;
    /// The executable file name of that process, read when the change happened.
    std::string image;
    bool ours = false; ///< the hand-over's window
};

/// What a hand-over did, with its time points (steady clock).
struct HandoverReport {
    WindowState before;      ///< at prepare()
    WindowState afterReturn; ///< at the last restore() or update()
    std::optional<std::chrono::steady_clock::time_point> prepared;
    std::optional<std::chrono::steady_clock::time_point> handedOver;
    std::optional<std::chrono::steady_clock::time_point> restoreBegun;
    /// The window is no longer minimized (at once for HandoverPolicy::Keep).
    std::optional<std::chrono::steady_clock::time_point> restored;
    /// It is in fullscreen and covers its monitor again (or it was not in fullscreen before the launch).
    std::optional<std::chrono::steady_clock::time_point> fullscreen;
    /// It is the foreground window.
    std::optional<std::chrono::steady_clock::time_point> foreground;
    /// SDL_RestoreWindow left the window minimized (SDL 3.4 ignores it for fullscreen windows): ShowWindow restored it.
    bool restoredByWin32 = false;
    /// Fullscreen had to be applied again: the restored window no longer covered its monitor.
    bool fullscreenReapplied = false;
    /// The window had come back on another monitor (a display mode change moved it while minimized) and was moved
    /// back to its display.
    bool movedBackToDisplay = false;
    /// The display's mode at restore() differs from the one at prepare() (an emulator's exclusive mode still set).
    bool displayModeChanged = false;
    std::vector<ForegroundAttempt> attempts;
    /// Foreground changes from prepare() on (the emulator taking it, Windows or a remedy giving it back), at most 64.
    std::vector<ForegroundChange> foregroundChanges;
    /// Every remedy failed: the taskbar button flashes and the user has to come back.
    bool userActionNeeded = false;
};

/// Hands Retroverse's window over to an emulator and takes it back (specification §11.4: "Minimize/Hide Retroverse",
/// "Recover foreground/window/display state"). Main thread only; `window` must outlive the object. From prepare() on,
/// an EVENT_SYSTEM_FOREGROUND hook records the foreground changes (delivered while the thread pumps its messages, as
/// SDL does); at most one WindowHandover hooks at a time.
///
/// Order of a launch: prepare() before starting the process; handOver() once the process exists but before it runs
/// (rv::launcher::StartCallbacks::beforeResume: Retroverse is still the foreground process when it allows the emulator
/// to take the foreground); restore() once the emulator is gone; update() every frame until it returns true.
class WindowHandover {
public:
    /// How long a remedy has to work before update() tries the next one.
    static constexpr std::chrono::milliseconds kRemedyDelay{100};

    WindowHandover(SDL_Window* window, HandoverPolicy policy) noexcept;
    WindowHandover(const WindowHandover&) = delete;
    WindowHandover& operator=(const WindowHandover&) = delete;
    ~WindowHandover();

    /// Remembers the window's state and starts recording foreground changes.
    void prepare();
    /// Applies the policy (Minimize: SDL_MinimizeWindow). Calls prepare() first when it was not called.
    void handOver();
    /// prepare() and handOver() at once.
    void leave();
    /// After the emulator is gone: restores a minimized window (SDL_RestoreWindow, then ShowWindow(SW_RESTORE) when
    /// SDL left it minimized), then raises it (SDL_RaiseWindow) unless it is in the foreground already; update()
    /// follows the rest of the return.
    void restore();
    /// After restore(), every frame: records when the window is restored, back in fullscreen over the monitor it was
    /// on (applying borderless fullscreen again, once, when the restored window does not cover it or came back on
    /// another monitor, which it is moved off) and in the foreground (whoever put it there, the user included);
    /// while it is not in the foreground, tries the next remedy once the previous one has had kRemedyDelay
    /// (AttachThreadInput, then AltKey, then Flash, which stays until the user comes back). True once restored, in
    /// fullscreen and in the foreground.
    bool update();
    /// GetForegroundWindow() is this window.
    [[nodiscard]] bool isForeground() const noexcept;

    [[nodiscard]] HandoverPolicy policy() const noexcept { return m_policy; }
    [[nodiscard]] const HandoverReport& report() const noexcept { return m_report; }
    /// The window's HWND.
    [[nodiscard]] void* nativeWindow() const noexcept { return m_hwnd; }

    /// Called by the event hook.
    void recordForegroundChange(void* hwnd);

private:
    void observe();
    void attempt(ForegroundRemedy remedy);
    void installHook();
    void removeHook() noexcept;

    SDL_Window* m_window = nullptr;
    void* m_hwnd = nullptr;
    HandoverPolicy m_policy = HandoverPolicy::Minimize;
    HandoverReport m_report;
    void* m_hook = nullptr;
    bool m_prepared = false;
    bool m_restoring = false;
    std::size_t m_nextRemedy = 0;
};

} // namespace rv::platform
