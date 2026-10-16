#include <rv/platform/window_handover.hpp>

#include <array>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <spdlog/spdlog.h>

#include <rv/core/text.hpp>

#include "win32_util.hpp"

namespace rv::platform {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxForegroundChanges = 64;
/// The remedies update() tries after restore()'s plain raise, in order.
constexpr std::array kRemedies{ForegroundRemedy::AttachThreadInput, ForegroundRemedy::AltKey,
                               ForegroundRemedy::Flash};

/// The hand-over whose hook is installed (WinEvent callbacks carry no context; they run on the hooking thread).
thread_local WindowHandover* t_hookOwner = nullptr;

void CALLBACK onForegroundChanged(HWINEVENTHOOK /*hook*/, DWORD /*event*/, HWND window, LONG objectId, LONG /*child*/,
                                  DWORD /*thread*/, DWORD /*time*/) {
    if (t_hookOwner != nullptr && objectId == OBJID_WINDOW) {
        try {
            t_hookOwner->recordForegroundChange(window);
        } catch (...) {
            // Called from the system: nothing may escape.
        }
    }
}

PixelRect toPixelRect(const RECT& rect) noexcept {
    return PixelRect{rect.left, rect.top, rect.right, rect.bottom};
}

/// File name of a process's executable, or empty.
std::string imageName(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return {};
    }
    std::array<wchar_t, 1024> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
        const std::wstring_view path(buffer.data(), size);
        const std::size_t slash = path.find_last_of(L"\\/");
        name = win32::narrow(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
    }
    CloseHandle(process);
    return name;
}

/// SetForegroundWindow with this thread's input attached to the foreground window's thread, which lifts the
/// foreground lock for it. Skipped for a hung foreground window (attaching to it would stall this thread's input).
void raiseAttached(HWND window) {
    const HWND foreground = GetForegroundWindow();
    const DWORD ourThread = GetCurrentThreadId();
    const DWORD foregroundThread = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    bool attached = false;
    if (foregroundThread != 0 && foregroundThread != ourThread && !IsHungAppWindow(foreground)) {
        attached = AttachThreadInput(ourThread, foregroundThread, TRUE) != FALSE;
    }
    BringWindowToTop(window);
    SetForegroundWindow(window);
    if (attached) {
        SetFocus(window);
        AttachThreadInput(ourThread, foregroundThread, FALSE);
    }
}

/// SetForegroundWindow between an injected Alt key press and release: Windows lets the target of a keyboard menu
/// interaction take the foreground.
void raiseWithAltKey(HWND window) {
    INPUT press{};
    press.type = INPUT_KEYBOARD;
    press.ki.wVk = VK_MENU;
    INPUT release = press;
    release.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &press, sizeof(INPUT));
    SetForegroundWindow(window);
    SendInput(1, &release, sizeof(INPUT));
}

void flash(HWND window) {
    FLASHWINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = window;
    info.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG; // until the window comes to the foreground
    FlashWindowEx(&info);
}

} // namespace

std::string_view toString(HandoverPolicy policy) noexcept {
    switch (policy) {
    case HandoverPolicy::Minimize:
        return "minimize";
    case HandoverPolicy::Keep:
        return "keep";
    }
    return "unknown";
}

std::optional<HandoverPolicy> parseHandoverPolicy(std::string_view text) noexcept {
    if (core::equalsIgnoreCaseAscii(text, "minimize")) {
        return HandoverPolicy::Minimize;
    }
    if (core::equalsIgnoreCaseAscii(text, "keep")) {
        return HandoverPolicy::Keep;
    }
    return std::nullopt;
}

std::string_view toString(ForegroundRemedy remedy) noexcept {
    switch (remedy) {
    case ForegroundRemedy::Raise:
        return "raise";
    case ForegroundRemedy::AttachThreadInput:
        return "attach-thread-input";
    case ForegroundRemedy::AltKey:
        return "alt-key";
    case ForegroundRemedy::Flash:
        return "flash";
    }
    return "unknown";
}

void* nativeWindowHandle(SDL_Window* window) noexcept {
    if (window == nullptr) {
        return nullptr;
    }
    return SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

WindowState captureWindowState(SDL_Window* window) {
    WindowState state;
    if (window == nullptr) {
        return state;
    }
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    state.fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    state.desktopFullscreen = state.fullscreen && SDL_GetWindowFullscreenMode(window) == nullptr;
    state.minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
    state.display = SDL_GetDisplayForWindow(window);
    if (const SDL_DisplayMode* mode = state.display != 0 ? SDL_GetCurrentDisplayMode(state.display) : nullptr) {
        state.displayWidth = mode->w;
        state.displayHeight = mode->h;
        state.displayRefreshRate = mode->refresh_rate;
    }
    const auto hwnd = static_cast<HWND>(nativeWindowHandle(window));
    if (hwnd != nullptr) {
        state.minimized = state.minimized || IsIconic(hwnd) != FALSE;
        state.foreground = GetForegroundWindow() == hwnd;
        RECT rect{};
        if (GetWindowRect(hwnd, &rect)) {
            state.windowRect = toPixelRect(rect);
        }
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            state.monitorRect = toPixelRect(monitor.rcMonitor);
        }
    }
    return state;
}

bool coversMonitor(const WindowState& state) noexcept {
    return state.fullscreen && !state.minimized && state.windowRect.width() > 0 &&
           state.windowRect == state.monitorRect;
}

WindowHandover::WindowHandover(SDL_Window* window, HandoverPolicy policy) noexcept
    : m_window(window), m_hwnd(nativeWindowHandle(window)), m_policy(policy) {}

WindowHandover::~WindowHandover() {
    removeHook();
}

bool WindowHandover::isForeground() const noexcept {
    return m_hwnd != nullptr && GetForegroundWindow() == static_cast<HWND>(m_hwnd);
}

void WindowHandover::installHook() {
    if (m_hook != nullptr || t_hookOwner != nullptr) {
        return;
    }
    m_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &onForegroundChanged, 0, 0,
                             WINEVENT_OUTOFCONTEXT);
    if (m_hook != nullptr) {
        t_hookOwner = this;
    } else {
        spdlog::warn("Window hand-over: no foreground event hook ({})", win32::lastErrorMessage());
    }
}

void WindowHandover::removeHook() noexcept {
    if (m_hook != nullptr) {
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_hook));
        m_hook = nullptr;
    }
    if (t_hookOwner == this) {
        t_hookOwner = nullptr;
    }
}

void WindowHandover::recordForegroundChange(void* hwnd) {
    if (m_report.foregroundChanges.size() >= kMaxForegroundChanges) {
        return;
    }
    ForegroundChange change;
    change.at = Clock::now();
    DWORD processId = 0;
    GetWindowThreadProcessId(static_cast<HWND>(hwnd), &processId);
    change.processId = processId;
    change.ours = hwnd == m_hwnd;
    change.image = change.ours ? std::string("(this window)") : imageName(processId);
    m_report.foregroundChanges.push_back(std::move(change));
}

void WindowHandover::prepare() {
    m_report = HandoverReport{};
    m_report.foregroundChanges.reserve(kMaxForegroundChanges);
    m_restoring = false;
    m_nextRemedy = 0;
    m_report.before = captureWindowState(m_window);
    m_report.prepared = Clock::now();
    installHook();
    m_prepared = true;
    if (!m_report.before.foreground) {
        spdlog::warn("Window hand-over: Retroverse is not in the foreground at the launch; the emulator may not be "
                     "allowed to take it");
    }
}

void WindowHandover::handOver() {
    if (!m_prepared) {
        prepare();
    }
    m_report.handedOver = Clock::now();
    if (m_policy == HandoverPolicy::Minimize && m_window != nullptr) {
        if (!SDL_MinimizeWindow(m_window)) {
            spdlog::warn("Window hand-over: SDL_MinimizeWindow failed: {}", SDL_GetError());
        }
        SDL_SyncWindow(m_window);
    }
    const WindowState& before = m_report.before;
    spdlog::info("Window hand-over: {} ({} {}x{} at {}x{} {:.0f} Hz, {})", toString(m_policy),
                 before.fullscreen ? "fullscreen" : "windowed", before.windowRect.width(), before.windowRect.height(),
                 before.displayWidth, before.displayHeight, before.displayRefreshRate,
                 before.foreground ? "foreground" : "background");
}

void WindowHandover::leave() {
    prepare();
    handOver();
}

void WindowHandover::restore() {
    if (m_window == nullptr || m_hwnd == nullptr) {
        return;
    }
    const auto hwnd = static_cast<HWND>(m_hwnd);
    m_restoring = true;
    m_report.restoreBegun = Clock::now();

    // 1. Out of the taskbar. SDL 3.4 ignores SDL_RestoreWindow for fullscreen windows (it expects them to come back
    // through activation): Windows restores it then, SDL re-enters fullscreen on the restored event.
    if (IsIconic(hwnd)) {
        if (!SDL_RestoreWindow(m_window)) {
            spdlog::warn("Window hand-over: SDL_RestoreWindow failed: {}", SDL_GetError());
        }
        if (IsIconic(hwnd)) {
            m_report.restoredByWin32 = true;
            ShowWindow(hwnd, SW_RESTORE);
        }
        SDL_SyncWindow(m_window);
    }

    // 2. The display's mode: an emulator's exclusive mode should be gone with it.
    const WindowState state = captureWindowState(m_window);
    const WindowState& before = m_report.before;
    m_report.displayModeChanged = m_prepared && (state.displayWidth != before.displayWidth ||
                                                 state.displayHeight != before.displayHeight ||
                                                 state.displayRefreshRate != before.displayRefreshRate);
    if (m_report.displayModeChanged) {
        spdlog::warn("Window hand-over: the display mode changed while the emulator ran ({}x{} {:.0f} Hz, was {}x{} "
                     "{:.0f} Hz)",
                     state.displayWidth, state.displayHeight, state.displayRefreshRate, before.displayWidth,
                     before.displayHeight, before.displayRefreshRate);
    }

    // 3. Fullscreen and foreground: what is reached now is recorded; a plain raise, the remedies follow in update().
    observe();
    if (!m_report.foreground) {
        attempt(ForegroundRemedy::Raise);
        observe();
    }
}

void WindowHandover::observe() {
    const auto hwnd = static_cast<HWND>(m_hwnd);
    const Clock::time_point now = Clock::now();
    WindowState state = captureWindowState(m_window);
    if (!m_report.restored && !state.minimized) {
        m_report.restored = now;
    }
    if (m_report.restored && !m_report.fullscreen) {
        const WindowState& before = m_report.before;
        // A display mode change can shrink the window, or move it to another monitor while it is minimized.
        const auto backInPlace = [&](const WindowState& current) {
            return coversMonitor(current) && (!m_prepared || current.monitorRect == before.monitorRect);
        };
        if (!before.fullscreen || backInPlace(state)) {
            m_report.fullscreen = now;
        } else if (!m_report.fullscreenReapplied) {
            m_report.fullscreenReapplied = true;
            const bool otherMonitor = m_prepared && state.monitorRect != before.monitorRect;
            spdlog::info("Window hand-over: the window covers {}x{} of {} {}x{} monitor; applying fullscreen again",
                         state.windowRect.width(), state.windowRect.height(), otherMonitor ? "another" : "its",
                         state.monitorRect.width(), state.monitorRect.height());
            SDL_SetWindowFullscreenMode(m_window, nullptr);
            SDL_SetWindowFullscreen(m_window, false);
            SDL_SyncWindow(m_window);
            if (otherMonitor && before.display != 0) {
                m_report.movedBackToDisplay = true;
                SDL_SetWindowPosition(m_window, static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(before.display)),
                                      static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(before.display)));
                SDL_SyncWindow(m_window);
            }
            SDL_SetWindowFullscreen(m_window, true);
            SDL_SyncWindow(m_window);
            state = captureWindowState(m_window);
            if (backInPlace(state)) {
                m_report.fullscreen = Clock::now();
            } else {
                spdlog::warn("Window hand-over: the window does not cover its monitor after applying fullscreen again");
            }
        }
    }
    if (!m_report.foreground && hwnd != nullptr && GetForegroundWindow() == hwnd) {
        m_report.foreground = now;
    }
    m_report.afterReturn = state;
}

void WindowHandover::attempt(ForegroundRemedy remedy) {
    const auto hwnd = static_cast<HWND>(m_hwnd);
    switch (remedy) {
    case ForegroundRemedy::Raise:
        SDL_RaiseWindow(m_window);
        break;
    case ForegroundRemedy::AttachThreadInput:
        raiseAttached(hwnd);
        break;
    case ForegroundRemedy::AltKey:
        raiseWithAltKey(hwnd);
        break;
    case ForegroundRemedy::Flash:
        flash(hwnd);
        m_report.userActionNeeded = true;
        spdlog::warn("Window hand-over: Windows refused the foreground to Retroverse; its taskbar button flashes "
                     "until the user returns to it");
        break;
    }
    ForegroundAttempt record;
    record.remedy = remedy;
    record.at = Clock::now();
    record.foregroundAfter = isForeground();
    m_report.attempts.push_back(record);
    if (record.foregroundAfter && !m_report.foreground) {
        m_report.foreground = record.at;
    }
    spdlog::info("Window hand-over: {} -> {}", toString(remedy),
                 record.foregroundAfter ? "in the foreground" : "not in the foreground");
}

bool WindowHandover::update() {
    if (!m_restoring) {
        return isForeground();
    }
    observe();
    const bool done = m_report.restored && m_report.fullscreen && m_report.foreground;
    if (m_report.foreground || m_nextRemedy >= kRemedies.size()) {
        return done;
    }
    if (!m_report.attempts.empty() && Clock::now() - m_report.attempts.back().at < kRemedyDelay) {
        return done;
    }
    attempt(kRemedies[m_nextRemedy++]);
    observe();
    return m_report.restored && m_report.fullscreen && m_report.foreground;
}

} // namespace rv::platform
