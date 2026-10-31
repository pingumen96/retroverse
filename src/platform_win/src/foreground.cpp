#include <rv/platform_win/foreground.hpp>

#include <rv/platform_win/win32_util.hpp>

namespace rv::platform_win {

ForegroundWindow foregroundWindow() noexcept {
    ForegroundWindow foreground;
    const HWND window = GetForegroundWindow();
    if (window != nullptr) {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        foreground.window = reinterpret_cast<std::uintptr_t>(window);
        foreground.processId = processId;
    }
    return foreground;
}

bool giveForeground(std::uintptr_t window) noexcept {
    const HWND target = reinterpret_cast<HWND>(window);
    if (target == nullptr || !IsWindow(target)) {
        return false;
    }
    const HWND foreground = GetForegroundWindow();
    const DWORD ourThread = GetCurrentThreadId();
    const DWORD foregroundThread = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    bool attached = false;
    if (foregroundThread != 0 && foregroundThread != ourThread && !IsHungAppWindow(foreground)) {
        attached = AttachThreadInput(ourThread, foregroundThread, TRUE) != FALSE;
    }
    BringWindowToTop(target);
    SetForegroundWindow(target);
    if (attached) {
        AttachThreadInput(ourThread, foregroundThread, FALSE);
    }
    return GetForegroundWindow() == target;
}

} // namespace rv::platform_win
