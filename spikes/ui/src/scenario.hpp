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
    Press,          ///< press a pad button
    Release,        ///< release a pad button
    Stick,          ///< move the left stick to `value` (rv::input conventions: +Y up)
    Resize,         ///< resize the window to `value` (window units)
    Text,           ///< type `text` (an SDL text input event, as a keyboard with the focus on a text field sends)
    Key,            ///< press and release the key `scancode` / `keycode`, `count` times (SDL key events)
    ExpectFocus,    ///< the focused element is `text`
    ExpectSteady,   ///< exactly `count` focus changes since frame `since` (0: the focus did not move)
    ExpectLaunched, ///< games launched so far, in order, separated by commas: `text`
    ExpectMedia,    ///< games whose media were asked so far: `text`
    ExpectInfo,     ///< the card's details are shown ("shown") or hidden ("hidden"): `text`
    ExpectHistory,  ///< the focus history holds `count` entries
    ExpectVisible,  ///< the list shows `count` games
    Layout,         ///< record the layout (element boxes) under `label`, with its checks
    SameLayout,     ///< the layout labelled `label` equals the one labelled `text`
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
};

/// A named span of frames [begin, end) whose UI times are summarized on their own in the report.
struct Phase {
    std::string name;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

/// The scripted run of spike_ui: the task's sequence (focus down 10 times, right into the card's actions, Avvia,
/// Indietro), then Y and X, a held stick (repeat), a stick resting near its threshold and a diagonal one (stable
/// focus), up past the top of the list into the filters and the search field, a search typed on the keyboard and
/// erased, a filter chosen with A and back, a resize to 1280x720 and back to 1920x1080 with the layout recorded, then
/// the measurement phases: "idle" (no input) and "navigation" (the D-pad held down and up: the focus moves every few
/// frames, the list scrolls, the card changes).
struct Scenario {
    std::vector<Step> steps; ///< sorted by frame
    std::vector<Phase> phases;
    /// Frames the scenario needs to run to its end.
    std::uint32_t frames = 0;
};

/// The scenario above, its expectations written for the mock catalog (mockCatalog) and a 1920x1080 window.
[[nodiscard]] Scenario defaultScenario();

[[nodiscard]] std::string_view toString(StepKind kind) noexcept;

} // namespace rv::spike_ui
