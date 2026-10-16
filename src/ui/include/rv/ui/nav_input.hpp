#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <rv/input/codes.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/ui/ui_command.hpp>

namespace rv::ui {

/// Tuning of focus navigation from the pad and the keyboard. The defaults follow style §16.2 (focus feedback under
/// 100 ms, a stable focus without jitter with the controller) and the habits of TV interfaces.
struct NavSettings {
    /// Left stick deflection (its length, in [0, 1]) that engages a direction...
    float stickEngage = 0.55f;
    /// ...and below which an engaged direction is released. Between the two the stick keeps its state (hysteresis): a
    /// stick resting near either threshold, or drifting, never toggles a direction on and off.
    float stickRelease = 0.35f;
    /// While a direction is engaged, the stick turns to the other axis only when that axis' deflection exceeds the
    /// engaged axis' by this factor (1.25: about 51 degrees off the engaged axis instead of 45), so a stick held near a
    /// diagonal keeps its direction instead of alternating.
    float axisSwitchRatio = 1.25f;
    /// A held direction steps once at once, again after this delay (seconds)...
    double repeatDelay = 0.40;
    /// ...then once per interval...
    double repeatInterval = 0.10;
    /// ...and once per fast interval after being held this long (long lists).
    double fastRepeatAfter = 1.50;
    double fastRepeatInterval = 0.05;
};

/// Turns a held direction into focus steps: one at once when a direction starts or changes, one more after
/// NavSettings::repeatDelay, then one per repeatInterval (fastRepeatInterval once held for fastRepeatAfter). At most
/// one step per frame and no backlog: a long frame (a hitch, a loading screen) never makes the focus jump several
/// places. Deterministic: driven by the frame deltas it is given (--fixed-dt makes runs reproducible).
class DirectionRepeater {
public:
    explicit DirectionRepeater(const NavSettings& settings = {}) noexcept;

    /// Advances by `deltaSeconds` (negative or non-finite counts as 0) with `held` the direction held during this
    /// frame (nothing: released). Returns the step of this frame, if any.
    [[nodiscard]] std::optional<NavDirection> update(std::optional<NavDirection> held, double deltaSeconds) noexcept;
    /// Forgets the held direction: the next held direction steps at once.
    void reset() noexcept;

    [[nodiscard]] std::optional<NavDirection> held() const noexcept { return m_held; }

private:
    NavSettings m_settings;
    std::optional<NavDirection> m_held;
    double m_heldSeconds = 0.0;   ///< since the direction started
    double m_nextStepAt = 0.0;    ///< m_heldSeconds of the next repeat
};

/// The left stick as a direction (rv::input conventions: x right, y up, each in [-1, 1]), with the hysteresis of
/// NavSettings: engaged at stickEngage, released below stickRelease, turned to the other axis only past
/// axisSwitchRatio.
class StickDirection {
public:
    explicit StickDirection(const NavSettings& settings = {}) noexcept;

    /// The direction the stick holds after this sample. Non-finite components count as 0.
    std::optional<NavDirection> update(float x, float y) noexcept;
    [[nodiscard]] std::optional<NavDirection> direction() const noexcept { return m_direction; }
    void reset() noexcept { m_direction.reset(); }

private:
    NavSettings m_settings;
    std::optional<NavDirection> m_direction;
};

/// Which pad buttons and keys give which command (specification §12.1 defaults). Directions also come from the left
/// stick when leftStickNavigates. A command may have several sources; empty lists disable it.
struct NavBindings {
    std::array<std::vector<input::GamepadButton>, kUiCommandCount> buttons;
    std::array<std::vector<input::Key>, kUiCommandCount> keys;
    bool leftStickNavigates = true;

    /// D-pad and arrow keys move the focus, A / Enter activates, B / Esc goes back, Y "Vedi media", X "Informazioni".
    /// Letters and the space bar stay free for text fields; Tab is left to the UI's own tab navigation.
    [[nodiscard]] static NavBindings defaults();
};

/// The commands of a frame from the raw device input (rv::input::RawInputFrame, filled by rv_platform's SDL
/// collector):
///  - focus moves: one DirectionRepeater for every source, so a held D-pad, arrow key or stick repeats alike. The
///    digital sources (D-pad, keys) win over the stick, and among them the direction pressed last; a direction tapped
///    within one frame (pressed and released before it ended) steps once;
///  - the other commands on the press of a bound button or key, after the frame's step (Down then Activate activates
///    the element the focus moved to).
/// Gamepad fields are ignored while RawInputFrame::gamepadConnected is false. Main thread, one update() per frame.
class NavInput {
public:
    explicit NavInput(NavSettings settings = {}, NavBindings bindings = NavBindings::defaults());

    /// Appends the commands of `frame` to `out`.
    void update(const input::RawInputFrame& frame, double deltaSeconds, UiCommandList& out);
    /// Forgets held directions and the press order (focus loss, a screen opening while the stick walks): directions
    /// held now step only once everything is released or a direction is pressed again, so a stick still held from
    /// walking does not start scrolling the screen that just opened. Buttons work at once.
    void reset() noexcept;

    [[nodiscard]] const NavSettings& settings() const noexcept { return m_settings; }
    [[nodiscard]] const NavBindings& bindings() const noexcept { return m_bindings; }

private:
    /// The digital direction held at the end of the frame, the most recently pressed one.
    [[nodiscard]] std::optional<NavDirection> heldDigital(const input::RawInputFrame& frame) const noexcept;
    /// A direction pressed and released within the frame, if any (the first found, in Up, Down, Left, Right order).
    [[nodiscard]] std::optional<NavDirection> tappedDigital(const input::RawInputFrame& frame) const noexcept;
    [[nodiscard]] bool isDown(const input::RawInputFrame& frame, UiCommand command) const noexcept;
    [[nodiscard]] bool wasPressed(const input::RawInputFrame& frame, UiCommand command) const noexcept;

    NavSettings m_settings;
    NavBindings m_bindings;
    DirectionRepeater m_repeater;
    StickDirection m_stick;
    /// Order of the last press of each direction (0: never), from m_pressCounter.
    std::array<std::uint64_t, 4> m_pressOrder{};
    std::uint64_t m_pressCounter = 0;
    /// Set by reset(): held directions give no step until released or pressed again.
    bool m_suppressHeld = false;
};

} // namespace rv::ui
