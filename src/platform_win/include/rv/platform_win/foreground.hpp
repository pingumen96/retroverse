#pragma once

// The foreground window for a game launched in VR (I10 step 10): the emulator's window is given the foreground once it
// shows, since most emulators read the pad only while their window is in front, and Retroverse, rendering on for the
// headset, keeps its own window where it was. Windows-only; main thread.

#include <cstdint>

namespace rv::platform_win {

/// The foreground window (an HWND as an integer; 0: none) and the process that owns it.
struct ForegroundWindow {
    std::uintptr_t window = 0;
    std::uint32_t processId = 0;

    bool operator==(const ForegroundWindow&) const = default;
};

[[nodiscard]] ForegroundWindow foregroundWindow() noexcept;

/// Brings `window` (an HWND) to the top and the foreground, this thread's input attached to the foreground window's
/// thread for the call, which lifts the foreground lock (as the window hand-over takes Retroverse's window back); never
/// attached to a hung window. True when `window` is the foreground window afterwards.
bool giveForeground(std::uintptr_t window) noexcept;

} // namespace rv::platform_win
