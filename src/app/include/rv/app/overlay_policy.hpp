#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstdint>
#include <optional>

#include <glm/vec2.hpp>

#include <rv/app/command_line.hpp>

namespace rv::app {

/// A frame-limited run whose result shows the developer overlay (--devui-open, --screenshot-include-ui; an input
/// script may also open it): in developer builds such a run fails (exit code 3) when the overlay cannot start, rather
/// than passing with screenshots of the scene alone. Interactive runs go on without the overlay.
[[nodiscard]] bool overlayRequired(const CommandLineOptions& options) noexcept;

/// Size of the screenshot the developer overlay is drawn into on frame `frameIndex` (counted from 0), with
/// --screenshot-include-ui: --screenshot-size when the frame takes an F12 screenshot (`screenshotRequested`) or the
/// --screenshot of the last frame of a frame-limited run. Nullopt when the frame takes no screenshot, and always
/// without --screenshot-include-ui.
[[nodiscard]] std::optional<glm::ivec2> overlayCaptureSize(const CommandLineOptions& options, std::uint64_t frameIndex,
                                                           bool screenshotRequested) noexcept;

} // namespace rv::app
