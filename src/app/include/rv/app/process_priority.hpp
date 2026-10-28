#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// Which priority class retroverse.exe runs at (I5 step 2, ADR-0016 §2, ADR-0004 point 9): above normal while its 3D
// view renders and the window is not minimized; normal while minimized and during a game (from just before the
// emulator starts until the return), so that an emulator never competes with a boosted Retroverse. The app sets it
// through rv_platform_win and records each change in the run report (ProcessPriorityRecord).

#include <string_view>

namespace rv::app {

struct PriorityChoice {
    bool aboveNormal = false;
    /// The run report's reason: "rendering", "minimized", "launch".
    std::string_view reason;

    bool operator==(const PriorityChoice&) const = default;
};

/// The priority for a frame: `gameRunning` (a launch active, its emulator about to start or running) first, then
/// `minimized`, else rendering.
[[nodiscard]] PriorityChoice processPriorityFor(bool minimized, bool gameRunning) noexcept;

} // namespace rv::app
