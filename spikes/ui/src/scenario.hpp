#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/ui/catalog_source.hpp>

namespace rv::spike_ui {

/// What a scenario step does.
enum class StepKind : std::uint8_t {
    Press,        ///< press a pad button
    Release,      ///< release a pad button
    Stick,        ///< move the left stick to `value` (rv::input conventions: +Y up)
    Trigger,      ///< move a trigger (`axis`) to `value.x` in [0, 1]
    Resize,       ///< resize the window to `value` (window units)
    Text,         ///< type `text` (an SDL text input event, as a keyboard with the focus on a text field sends)
    Key,          ///< press and release the key `scancode` / `keycode`, `count` times (SDL key events)
    TextScale,    ///< set the view's text scale to `value.x` times the run's (--ui-scale)
    HighContrast, ///< the high-contrast theme: `count` 1 on, 0 off, 2 the run's setting (--high-contrast)
    Mark,         ///< record the selected position and game under `label`
    /// An import changes the catalog under the screen: the source's entries are replaced and the model refreshed
    /// directly, never through the screen (as the application does when an import completes), the source answering the
    /// query `position` polls later (as on the catalog worker). `count` games just before the selection are removed, or
    /// (`count` 0) the games the last import removed come back. The list must be the whole one (no search, no filter).
    Import,
    ExpectFocus,       ///< the focused element is `text`
    /// Exactly `count` moves of the selection since frame `since` (0: it did not move), and as many moves of the focus:
    /// in the list the focus follows the selected game's row, and nothing else moves it.
    ExpectSteady,
    ExpectLaunched,    ///< games launched so far, in order, separated by commas: `text`
    ExpectMedia,       ///< games whose media were asked so far: `text`
    ExpectInfo,        ///< the card's details are shown ("shown") or hidden ("hidden"): `text`
    ExpectHistory,     ///< the focus history holds `count` entries
    ExpectCount,       ///< the list holds `count` games
    ExpectSelected,    ///< the selection is at position `position` of the list (and on the game `text`, if given)
    ExpectPages,       ///< the selection is `pages` pages (of the window's rows) from the position marked `label`
    ExpectMarked,      ///< the selection is on the game marked `label`, `pages` positions from where it was marked
    ExpectRow,         ///< the focus is on the row of the selected game, which shows its title and has "selected"
    ExpectWindow,      ///< the pool shows the model's window: the rows that fit, their titles, blanks, the scroll thumb
    ExpectCard,        ///< the card's title shows the selected game
    ExpectInView,      ///< the box of element `text` lies inside the box of element `label` (scrolled into view)
    ExpectClass,       ///< element `text` has (`count` 1) or has not (0) the class `label`
    ExpectOverflowCue, ///< the fades of the chips row match the chips that lie beyond its edges
    ExpectHighContrast, ///< the theme is on (`count` 1), off (0) or the run's setting (2): body class, black panels
    Layout,             ///< record the layout (element boxes, font sizes) under `label`, with its checks
    SameLayout,         ///< the layout labelled `label` equals the one labelled `text`
    LargerText,         ///< the font sizes of layout `label` are `value.x` times those of layout `text`
};

/// One step of a scripted scenario, run during the frame `frame` (counted from 0). Pad state set during a frame
/// reaches the screen with the next frame's events, so checks come a few frames after the input they verify.
struct Step {
    std::uint32_t frame = 0;
    StepKind kind = StepKind::Press;
    SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
    SDL_GamepadAxis axis = SDL_GAMEPAD_AXIS_INVALID;
    glm::vec2 value{0.0f};
    std::string text;
    std::string label;
    std::uint32_t since = 0;
    std::uint32_t count = 0;
    std::size_t position = 0;
    int pages = 0;
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
/// the focus and lays the card out again in the same frame) and "navigation" (the D-pad held: the selection moves every
/// few frames and the window scrolls with it, the card follows once it rests).
inline constexpr std::string_view kIdlePhase = "idle";
inline constexpr std::string_view kTapsPhase = "taps";
inline constexpr std::string_view kNavigationPhase = "navigation";

/// The scripted scenarios of spike_ui (--scenario).
enum class ScenarioKind : std::uint8_t {
    QuickCatalog, ///< "quick-catalog", the default: the whole screen over the 100,000-game mock (defaultScenario)
    UserBindings, ///< "user-bindings": a user bindings.json reaches the UI (userBindingsScenario)
};

[[nodiscard]] std::string_view toString(ScenarioKind kind) noexcept;
[[nodiscard]] std::optional<ScenarioKind> scenarioKindFromName(std::string_view name) noexcept;

/// A scripted run of spike_ui.
struct Scenario {
    std::vector<Step> steps; ///< sorted by frame
    std::vector<Phase> phases;
    /// Frames the scenario needs to run to its end.
    std::uint32_t frames = 0;
};

/// What the quick-catalog scenario expects of the catalog, read from the source before the run (readScenarioFacts):
/// positions are those of the full list (no search, every platform) unless named otherwise.
struct ScenarioFacts {
    std::size_t count = 0;
    /// The ids of the first positions of the list (the games the scenario launches and asks the media of).
    std::vector<std::string> ids;
    /// Filter chips: every game and one per platform.
    std::size_t chips = 0;
    /// The marker: the word that finds it alone (mockCatalog's kMarkerWord), its position, its platform's chip (the
    /// first), how many games that platform holds and the marker's position among them.
    std::string markerSearch;
    std::size_t marker = 0;
    std::string markerId;
    std::size_t markerChip = 0;
    std::size_t markerChipCount = 0;
    std::size_t markerInChip = 0;
    /// A word of the marker's title (ASCII letters), the games it finds and the marker's position among them.
    std::string markerWord;
    std::size_t markerWordCount = 0;
    std::size_t markerInWord = 0;
    /// The games of the platform of chip 2 (the second platform).
    std::size_t secondChipCount = 0;
    /// The position in the full list of the first game of chip 2's platform.
    std::size_t secondChipFirstInAll = 0;
};

/// Reads the facts of the quick-catalog scenario from `source` (queries run on it; it is left with the blank query).
/// `marker` is the marker's id. Errors: the source fails, or it lacks what the scenario needs (the marker, two
/// platforms with two games or more each, the marker on the first platform).
[[nodiscard]] Expected<ScenarioFacts> readScenarioFacts(ui::CatalogSource& source, std::string_view markerWord);

/// The quick-catalog scenario: the task's sequence (selection down 10 times, right into the card's actions, Avvia,
/// Indietro), then Y and X, a held stick (repeat), a stick resting near its threshold and a diagonal one (stable
/// focus), up past the top of the list into the filters and the search field, the marker searched on the keyboard and
/// the list restored on it in the middle, a word of its title narrowing the list around it, a filter chosen with A and
/// back, Home and a resize to 1280x720 and back to 1920x1080 with the layout recorded; then a stick crossing its
/// threshold every frame, the last filter chip scrolled into view with the row's fades, a filter that keeps the same
/// games, text scaling and high contrast; then the shoulders (filters keeping or moving the selection), End, the
/// triggers' pages, Home and a held trigger; then the measurement phases (kIdlePhase, kTapsPhase, kNavigationPhase);
/// after them, imports that change the catalog under the screen (the list of a query answered on a worker) and a list
/// that empties while the focus is on the card's actions. Its expectations follow `facts`, a 1920x1080 window and the
/// default --ui-scale and --high-contrast.
[[nodiscard]] Scenario defaultScenario(const ScenarioFacts& facts);

/// The user-bindings scenario, for a user bindings.json that binds ui_down to the pad's X (west) alone and unbinds
/// ui_info (spikes/ui/fixtures/user_bindings.json): the D-pad down no longer moves the selection, X moves it down and
/// shows no details, the D-pad up (a default binding) still moves it up.
[[nodiscard]] Scenario userBindingsScenario();

[[nodiscard]] std::string_view toString(StepKind kind) noexcept;
/// True for the steps that check something (the others drive the input or record).
[[nodiscard]] bool isCheck(StepKind kind) noexcept;

} // namespace rv::spike_ui
