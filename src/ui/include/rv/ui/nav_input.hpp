#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>

#include <rv/input/action.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/codes.hpp>
#include <rv/ui/ui_command.hpp>

namespace rv::ui {

/// Tuning of focus navigation from the pad and the keyboard. The defaults follow style §16.2 (focus feedback under
/// 100 ms, a stable focus without jitter with the controller) and the habits of TV interfaces.
struct NavSettings {
    /// Stick deflection (the length of (UiNavX, UiNavY), in [0, 1]) that engages a direction. The values are the
    /// actions', which the bindings' dead zone shapes: with the default bindings' 0.15 the left stick engages at 0.62
    /// and releases at 0.45 of its travel along an axis...
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

/// The action of the action map (input::Action::UiUp ...) that gives `command`: the user's bindings reach the UI
/// through them (ADR-0005).
[[nodiscard]] input::Action actionFor(UiCommand command) noexcept;

/// The keys bound to a UI action in `map` (every action input::isUiAction names, the stick's axes included): UiView
/// keeps them away from RmlUi's key handling, since they reach the UI as commands (the screen sends RmlUi's own keys
/// where an element uses them: the arrows, Home and End in a text field).
[[nodiscard]] std::bitset<input::kKeyCount> commandKeys(const input::ActionMap& map);

/// The commands of a frame from its actions (input::ActionState, resolved by input::InputSystem through the bindings
/// and overridden by --input-script):
///  - focus moves: one DirectionRepeater for every source, so a held D-pad, arrow key or stick repeats alike. The
///    digital directions (UiUp, UiDown, UiLeft, UiRight) win over the stick (UiNavX, UiNavY: StickDirection's
///    hysteresis on the values the bindings' dead zone shaped), and among them the direction pressed last; a direction
///    tapped within one frame (pressed and released before it ended) steps once;
///  - page moves (UiPageUp, UiPageDown): a DirectionRepeater of their own, the one pressed last winning;
///  - the other commands on the press of their action (Activate, Back, Media, Info, Home, End, FilterPrevious,
///    FilterNext, in that order), after the frame's steps (Down then Activate activates the element the focus moved
///    to).
/// A press is an edge of the action (InputSystem's): released and pressed again between two frames reads as held, as
/// for every action. Main thread, one update() per frame.
class NavInput {
public:
    explicit NavInput(NavSettings settings = {});

    /// Appends the commands of `actions` to `out`.
    void update(const input::ActionState& actions, double deltaSeconds, UiCommandList& out);
    /// Forgets held directions and pages and the press order (focus loss, a screen opening while the stick walks):
    /// directions held now step only once everything is released or a direction is pressed again, so a stick still held
    /// from walking does not start scrolling the screen that just opened; pages likewise. Buttons work at once.
    void reset() noexcept;

    [[nodiscard]] const NavSettings& settings() const noexcept { return m_settings; }

private:
    /// The digital direction held at the end of the frame, the most recently pressed one.
    [[nodiscard]] std::optional<NavDirection> heldDigital(const input::ActionState& actions) const noexcept;
    /// A direction pressed and released within the frame, if any (the first found, in Up, Down, Left, Right order).
    [[nodiscard]] std::optional<NavDirection> tappedDigital(const input::ActionState& actions) const noexcept;
    /// The page command held at the end of the frame (the most recently pressed one), or tapped within it.
    [[nodiscard]] std::optional<NavDirection> heldPage(const input::ActionState& actions) const noexcept;
    [[nodiscard]] std::optional<NavDirection> tappedPage(const input::ActionState& actions) const noexcept;
    /// The step of the directions this frame.
    [[nodiscard]] std::optional<NavDirection> directionStep(const input::ActionState& actions, double deltaSeconds);
    /// The page step this frame: Up for a page up, Down for a page down.
    [[nodiscard]] std::optional<NavDirection> pageStep(const input::ActionState& actions, double deltaSeconds);

    NavSettings m_settings;
    DirectionRepeater m_repeater;
    /// Page moves as directions: Up pages up, Down pages down.
    DirectionRepeater m_pageRepeater;
    StickDirection m_stick;
    /// Order of the last press of each direction and of each page command (0: never), from m_pressCounter.
    std::array<std::uint64_t, 4> m_pressOrder{};
    std::array<std::uint64_t, 2> m_pagePressOrder{};
    std::uint64_t m_pressCounter = 0;
    /// Set by reset(): held directions (pages) give no step until released or pressed again.
    bool m_suppressHeld = false;
    bool m_suppressPages = false;
};

} // namespace rv::ui
