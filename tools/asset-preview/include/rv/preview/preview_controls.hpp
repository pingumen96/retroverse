#pragma once

// GPU-free (part of rv_preview_core): no SDL, OpenGL or Dear ImGui header is included.

#include <optional>

#include <rv/input/raw_input.hpp>
#include <rv/preview/view_mode.hpp>
#include <rv/scene/orbit_controller.hpp>

namespace rv::preview {

/// Deadzone of the gamepad sticks in the orbit controls (that of the walking bindings).
inline constexpr float kStickDeadzone = 0.15f;

/// The orbit controls of one frame of raw input:
///  - mouse: a left drag orbits, a middle or a right drag pans (the frame's mouse motion is a drag's: the tool collects
///    motion only while such a button is held over the scene), the wheel zooms;
///  - gamepad (when one is connected): the right stick orbits, the left stick pans, the right trigger zooms in and the
///    left one out. The sticks have a radial deadzone of kStickDeadzone, rescaled so that motion starts at 0.
[[nodiscard]] scene::OrbitInput orbitInputFrom(const input::RawInputFrame& frame) noexcept;

/// The input moves the camera (any drag, wheel notch, stick or trigger): the user has taken it over.
[[nodiscard]] bool movesCamera(const scene::OrbitInput& input) noexcept;

/// The preview's shortcuts pressed in one frame.
struct PreviewShortcuts {
    bool frameSubject = false;     ///< F: frame the subject again
    bool toggleAutoRotate = false; ///< R: start or stop the auto-rotation
    /// 1 to 5: the view modes in kViewModes order.
    std::optional<ViewMode> viewMode;
};

/// The shortcuts of the keys pressed in `frame`.
[[nodiscard]] PreviewShortcuts previewShortcutsFrom(const input::RawInputFrame& frame) noexcept;

} // namespace rv::preview
