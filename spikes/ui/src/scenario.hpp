#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <glm/vec2.hpp>

namespace rv::spike_ui {

/// What a scenario step does.
enum class StepKind : std::uint8_t {
    Press,              ///< press a pad button
    Release,            ///< release a pad button
    Stick,              ///< move the left stick to `value` (rv::input conventions: +Y up)
    Resize,             ///< resize the window to `value` (window units)
    Text,               ///< type `text` (an SDL text input event, as a keyboard with the focus on a text field sends)
    Key,                ///< press and release the key `scancode` / `keycode`, `count` times (SDL key events)
    TextScale,          ///< set the view's text scale to `value.x` times the run's (--ui-scale)
    HighContrast,       ///< the high-contrast theme: `count` 1 on, 0 off, 2 the run's setting (--high-contrast)
    ExpectFocus,        ///< the focused element is `text`
    ExpectSteady,       ///< exactly `count` focus changes since frame `since` (0: the focus did not move)
    ExpectLaunched,     ///< games launched so far, in order, separated by commas: `text`
    ExpectMedia,        ///< games whose media were asked so far: `text`
    ExpectInfo,         ///< the card's details are shown ("shown") or hidden ("hidden"): `text`
    ExpectHistory,      ///< the focus history holds `count` entries
    ExpectVisible,      ///< the list shows `count` games
    ExpectCard,         ///< the card's title shows the selected game
    ExpectInView,       ///< the box of element `text` lies inside the box of element `label` (scrolled into view)
    ExpectClass,        ///< element `text` has (`count` 1) or has not (0) the class `label`
    ExpectOverflowCue,  ///< the fades of the chips row match the chips that lie beyond its edges
    ExpectHighContrast, ///< the theme is on (`count` 1), off (0) or the run's setting (2): body class, black panels
    Layout,             ///< record the layout (element boxes, font sizes) under `label`, with its checks
    SameLayout,         ///< the layout labelled `label` equals the one labelled `text`
    LargerText,         ///< the font sizes of layout `label` are `value.x` times those of layout `text`
};

/// One step of the scripted scenario, run during the frame `frame` (counted from 0). Pad state set during a frame
/// reaches the screen with the next frame's events, so checks come a few frames after the input they verify.
struct Step {
    std::uint32_t frame = 0;
    StepKind kind = StepKind::Press;
    SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
    glm::vec2 value{0.0f};
    std::string text;
    std::string label;
    std::uint32_t since = 0;
    std::uint32_t count = 0;
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    SDL_Keycode keycode = SDLK_UNKNOWN;
    /// The check verifies what keyboard input did (a typed search): named "keyboard-..." instead of "expect-...", so
    /// that the pad-only verdict leaves it out.
    bool keyboard = false;
};

/// A named span of frames [begin, end) whose UI times are summarized on their own in the report.
struct Phase {
    std::string name;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

/// The phases whose frames measure the UI pass: "idle" (no input), "taps" (single D-pad taps 0.3 s apart: each moves
/// the focus and lays the card out again in the same frame) and "navigation" (the D-pad held: the focus moves every few
/// frames, the card follows once it rests).
inline constexpr std::string_view kIdlePhase = "idle";
inline constexpr std::string_view kTapsPhase = "taps";
inline constexpr std::string_view kNavigationPhase = "navigation";

/// The scripted run of spike_ui: the task's sequence (focus down 10 times, right into the card's actions, Avvia,
/// Indietro), then Y and X, a held stick (repeat), a stick resting near its threshold and a diagonal one (stable
/// focus), up past the top of the list into the filters and the search field, a search typed on the keyboard and
/// erased, a filter chosen with A and back, a resize to 1280x720 and back to 1920x1080 with the layout recorded; then
/// a stick crossing its threshold every frame, the last filter chip scrolled into view with the row's fades, a filter
/// that keeps the same games, text scaling and high contrast; then the measurement phases (kIdlePhase, kTapsPhase,
/// kNavigationPhase).
struct Scenario {
    std::vector<Step> steps; ///< sorted by frame
    std::vector<Phase> phases;
    /// Frames the scenario needs to run to its end.
    std::uint32_t frames = 0;
};

/// The scenario above, its expectations written for the mock catalog (mockCatalog), a 1920x1080 window and the default
/// --ui-scale and --high-contrast.
[[nodiscard]] Scenario defaultScenario();

[[nodiscard]] std::string_view toString(StepKind kind) noexcept;
/// True for the steps that check something (the others drive the input).
[[nodiscard]] bool isCheck(StepKind kind) noexcept;

} // namespace rv::spike_ui
