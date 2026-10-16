#include "scenario.hpp"

#include <algorithm>
#include <utility>

namespace rv::spike_ui {
namespace {

/// Frames a scripted tap holds its button: well under the 0.4 s repeat delay, so one step per tap.
constexpr std::uint32_t kTapFrames = 3;

/// Builds the step list in any order; finish() sorts it by frame (stable: steps of one frame keep their order).
class ScenarioBuilder {
public:
    void press(std::uint32_t frame, SDL_GamepadButton button) { add(frame, StepKind::Press).button = button; }
    void release(std::uint32_t frame, SDL_GamepadButton button) { add(frame, StepKind::Release).button = button; }
    void tap(std::uint32_t frame, SDL_GamepadButton button) {
        press(frame, button);
        release(frame + kTapFrames, button);
    }
    void stick(std::uint32_t frame, float x, float y) { add(frame, StepKind::Stick).value = glm::vec2(x, y); }
    void resize(std::uint32_t frame, int width, int height) {
        add(frame, StepKind::Resize).value = glm::vec2(static_cast<float>(width), static_cast<float>(height));
    }
    void type(std::uint32_t frame, std::string text) { add(frame, StepKind::Text).text = std::move(text); }
    void key(std::uint32_t frame, SDL_Scancode scancode, SDL_Keycode keycode, std::uint32_t times) {
        Step& step = add(frame, StepKind::Key);
        step.scancode = scancode;
        step.keycode = keycode;
        step.count = times;
    }
    void expectFocus(std::uint32_t frame, std::string element) {
        add(frame, StepKind::ExpectFocus).text = std::move(element);
    }
    void expectSteady(std::uint32_t frame, std::uint32_t since, std::uint32_t changes) {
        Step& step = add(frame, StepKind::ExpectSteady);
        step.since = since;
        step.count = changes;
    }
    void expectLaunched(std::uint32_t frame, std::string games) {
        add(frame, StepKind::ExpectLaunched).text = std::move(games);
    }
    void expectMedia(std::uint32_t frame, std::string games) {
        add(frame, StepKind::ExpectMedia).text = std::move(games);
    }
    void expectInfo(std::uint32_t frame, bool shown) {
        add(frame, StepKind::ExpectInfo).text = shown ? "shown" : "hidden";
    }
    void expectHistory(std::uint32_t frame, std::uint32_t depth) { add(frame, StepKind::ExpectHistory).count = depth; }
    void expectVisible(std::uint32_t frame, std::uint32_t games) { add(frame, StepKind::ExpectVisible).count = games; }
    void layout(std::uint32_t frame, std::string label) { add(frame, StepKind::Layout).label = std::move(label); }
    void sameLayout(std::uint32_t frame, std::string label, std::string reference) {
        Step& step = add(frame, StepKind::SameLayout);
        step.label = std::move(label);
        step.text = std::move(reference);
    }
    void phase(std::string name, std::uint32_t begin, std::uint32_t end) {
        m_scenario.phases.push_back(Phase{std::move(name), begin, end});
    }

    Scenario finish(std::uint32_t frames) {
        std::stable_sort(m_scenario.steps.begin(), m_scenario.steps.end(),
                         [](const Step& a, const Step& b) { return a.frame < b.frame; });
        m_scenario.frames = frames;
        return std::move(m_scenario);
    }

private:
    Step& add(std::uint32_t frame, StepKind kind) {
        Step& step = m_scenario.steps.emplace_back();
        step.frame = frame;
        step.kind = kind;
        return step;
    }

    Scenario m_scenario;
};

} // namespace

Scenario defaultScenario() {
    constexpr SDL_GamepadButton kDown = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    constexpr SDL_GamepadButton kUp = SDL_GAMEPAD_BUTTON_DPAD_UP;
    constexpr SDL_GamepadButton kLeft = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    constexpr SDL_GamepadButton kRight = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    constexpr SDL_GamepadButton kA = SDL_GAMEPAD_BUTTON_SOUTH;
    constexpr SDL_GamepadButton kB = SDL_GAMEPAD_BUTTON_EAST;
    constexpr SDL_GamepadButton kX = SDL_GAMEPAD_BUTTON_WEST;
    constexpr SDL_GamepadButton kY = SDL_GAMEPAD_BUTTON_NORTH;

    ScenarioBuilder s;
    s.phase("startup", 0, 30);
    s.phase("sequence", 30, 700);
    s.phase("idle", 700, 1000);
    s.phase("navigation", 1000, 1300);

    // The task's sequence: focus down 10 times (the first game has the focus at start), right into the card's actions,
    // activate (Avvia), back (to the game the actions were opened from).
    for (std::uint32_t tap = 0; tap < 10; ++tap) {
        s.tap(30 + 8 * tap, kDown);
    }
    s.expectFocus(108, "game-g11");
    s.tap(110, kRight);
    s.expectFocus(116, "act-play");
    s.expectHistory(116, 1);
    s.tap(120, kA);
    s.expectLaunched(126, "g11");
    s.tap(130, kB);
    s.expectFocus(136, "game-g11");
    s.expectHistory(136, 0);

    // The secondary actions: Y "Vedi media", X "Informazioni" twice (shown, hidden).
    s.tap(140, kY);
    s.expectMedia(146, "g11");
    s.tap(150, kX);
    s.expectInfo(156, true);
    s.tap(160, kX);
    s.expectInfo(166, false);

    // The left stick held down for one second: one step at once, then one per 0.1 s after 0.4 s (7 steps).
    s.stick(170, 0.0f, -0.9f);
    s.stick(230, 0.0f, 0.0f);
    s.expectFocus(236, "game-g18");
    // A stick resting near half deflection (worn, or a thumb on it): the focus never moves.
    for (std::uint32_t frame = 240; frame < 360; ++frame) {
        s.stick(frame, 0.01f, frame % 2 == 0 ? -0.47f : -0.52f);
    }
    s.stick(360, 0.0f, 0.0f);
    s.expectSteady(364, 241, 0);
    s.expectFocus(364, "game-g18");
    // A stick near the diagonal: one step down, no turn to the right (it would leave the list for the actions).
    s.stick(370, 0.6f, -0.62f);
    s.stick(385, 0.66f, -0.6f);
    s.stick(392, 0.0f, 0.0f);
    s.expectSteady(398, 371, 1);
    s.expectFocus(398, "game-g19");

    // Up held: the list repeats faster after 1.5 s, reaches the top, crosses into the filters, then the search field.
    s.press(400, kUp);
    s.release(560, kUp);
    s.expectFocus(565, "search");
    s.expectHistory(565, 2);
    // The keyboard types into the focused search field (the list filters as it changes), Backspace erases it.
    s.type(566, "faro");
    s.expectVisible(567, 1);
    s.key(567, SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, 4);
    s.expectVisible(569, 20);
    // Down: back into the filters (Back would now return to the list).
    s.tap(570, kDown);
    s.expectFocus(576, "chip-0");
    s.expectHistory(576, 1);
    // Two filters to the right, A filters the list ("8 bit": 3 games), down lands on its first game.
    s.tap(580, kRight);
    s.tap(588, kRight);
    s.expectFocus(594, "chip-2");
    s.tap(596, kA);
    s.expectVisible(602, 3);
    s.tap(604, kDown);
    s.expectFocus(610, "game-g02");
    s.expectHistory(610, 0);
    s.tap(612, kDown);
    s.expectFocus(618, "game-g09");
    // Back up to the filters (they remember "8 bit"), left to "Tutte", A, down into the list where it was.
    s.tap(620, kUp);
    s.tap(628, kUp);
    s.expectFocus(634, "chip-2");
    s.tap(636, kLeft);
    s.tap(644, kLeft);
    s.expectFocus(650, "chip-0");
    s.tap(652, kA);
    s.expectVisible(658, 20);
    s.tap(660, kDown);
    s.expectFocus(666, "game-g02");

    // Resize: 1280x720, then back to 1920x1080, whose layout must equal the one before.
    s.layout(668, "1920x1080");
    s.resize(670, 1280, 720);
    s.layout(682, "1280x720");
    s.resize(684, 1920, 1080);
    s.layout(696, "1920x1080 again");
    s.sameLayout(696, "1920x1080 again", "1920x1080");

    // Measurements: idle, then the D-pad held down and up (the focus moves every 3 to 6 frames, the card changes).
    s.press(1000, kDown);
    s.release(1100, kDown);
    s.press(1100, kUp);
    s.release(1200, kUp);
    s.press(1200, kDown);
    s.release(1300, kDown);
    return s.finish(1320);
}

std::string_view toString(StepKind kind) noexcept {
    switch (kind) {
    case StepKind::Press:
        return "press";
    case StepKind::Release:
        return "release";
    case StepKind::Stick:
        return "stick";
    case StepKind::Resize:
        return "resize";
    case StepKind::Text:
        return "text";
    case StepKind::Key:
        return "key";
    case StepKind::ExpectFocus:
        return "expect-focus";
    case StepKind::ExpectSteady:
        return "expect-steady";
    case StepKind::ExpectLaunched:
        return "expect-launched";
    case StepKind::ExpectMedia:
        return "expect-media";
    case StepKind::ExpectInfo:
        return "expect-info";
    case StepKind::ExpectHistory:
        return "expect-history";
    case StepKind::ExpectVisible:
        return "expect-visible";
    case StepKind::Layout:
        return "layout";
    case StepKind::SameLayout:
        return "same-layout";
    }
    return "unknown";
}

} // namespace rv::spike_ui
