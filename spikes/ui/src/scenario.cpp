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
    void textScale(std::uint32_t frame, float factor) { add(frame, StepKind::TextScale).value = glm::vec2(factor); }
    /// 1 on, 0 off, 2 the run's setting.
    void highContrast(std::uint32_t frame, std::uint32_t state) { add(frame, StepKind::HighContrast).count = state; }
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
    /// `keyboard`: the count follows from typed text.
    void expectVisible(std::uint32_t frame, std::uint32_t games, bool keyboard = false) {
        Step& step = add(frame, StepKind::ExpectVisible);
        step.count = games;
        step.keyboard = keyboard;
    }
    void expectCard(std::uint32_t frame) { add(frame, StepKind::ExpectCard); }
    void expectInView(std::uint32_t frame, std::string element, std::string container) {
        Step& step = add(frame, StepKind::ExpectInView);
        step.text = std::move(element);
        step.label = std::move(container);
    }
    void expectClass(std::uint32_t frame, std::string element, std::string className, bool set) {
        Step& step = add(frame, StepKind::ExpectClass);
        step.text = std::move(element);
        step.label = std::move(className);
        step.count = set ? 1 : 0;
    }
    void expectOverflowCue(std::uint32_t frame) { add(frame, StepKind::ExpectOverflowCue); }
    /// 1 on, 0 off, 2 the run's setting.
    void expectHighContrast(std::uint32_t frame, std::uint32_t state) {
        add(frame, StepKind::ExpectHighContrast).count = state;
    }
    void layout(std::uint32_t frame, std::string label) { add(frame, StepKind::Layout).label = std::move(label); }
    void sameLayout(std::uint32_t frame, std::string label, std::string reference) {
        Step& step = add(frame, StepKind::SameLayout);
        step.label = std::move(label);
        step.text = std::move(reference);
    }
    void largerText(std::uint32_t frame, std::string label, std::string reference, float ratio) {
        Step& step = add(frame, StepKind::LargerText);
        step.label = std::move(label);
        step.text = std::move(reference);
        step.value = glm::vec2(ratio);
    }
    void phase(std::string_view name, std::uint32_t begin, std::uint32_t end) {
        m_scenario.phases.push_back(Phase{std::string(name), begin, end});
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
    constexpr std::uint32_t kOn = 1;
    constexpr std::uint32_t kRunSetting = 2;

    ScenarioBuilder s;
    s.phase("startup", 0, 30);
    s.phase("sequence", 30, 930);
    s.phase(kIdlePhase, 930, 1230);
    s.phase(kTapsPhase, 1230, 1590);
    s.phase(kNavigationPhase, 1590, 1890);

    // The task's sequence: focus down 10 times (the first game has the focus at start), right into the card's actions,
    // activate (Avvia), back (to the game the actions were opened from).
    for (std::uint32_t tap = 0; tap < 10; ++tap) {
        s.tap(30 + 8 * tap, kDown);
    }
    s.expectFocus(108, "game-g11");
    s.tap(110, kRight);
    // The downs came 8 frames apart, faster than the card follows (0.15 s): entering the actions fills it at once, so
    // Avvia shows the game it launches (the focus moves at frame 111; the card would catch up at 113 by itself).
    s.expectCard(112);
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
    // A stick resting near half deflection, under the engage threshold (a worn stick, a thumb on it): no step.
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
    s.expectVisible(567, 1, true);
    s.key(567, SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, 4);
    s.expectVisible(569, 20, true);
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

    // A stick crossing its engage threshold (0.55) every frame, 0.58 and 0.53: the hysteresis holds the direction, so it
    // steps at the held cadence (at once, again after 0.4 s), never on every crossing (a single threshold would step
    // every other frame).
    for (std::uint32_t frame = 700; frame < 728; ++frame) {
        s.stick(frame, 0.01f, frame % 2 == 0 ? -0.58f : -0.53f);
    }
    s.stick(728, 0.0f, 0.0f);
    s.expectSteady(734, 700, 2);
    s.expectFocus(734, "game-g04");

    // Up into the filters (they remember "Tutte"), then right to the last chip: the row scrolls it into view, and its
    // fades mark the edges chips hide beyond (the end of the row first, its start once scrolled).
    for (std::uint32_t tap = 0; tap < 4; ++tap) {
        s.tap(736 + 8 * tap, kUp);
    }
    s.expectFocus(766, "chip-0");
    s.expectOverflowCue(766);
    for (std::uint32_t tap = 0; tap < 5; ++tap) {
        s.tap(768 + 8 * tap, kRight);
    }
    s.expectFocus(806, "chip-5");
    s.expectInView(806, "chip-5", "chips");
    s.expectOverflowCue(806);

    // A filter that lets the same games through still moves the highlight: "faro" (typed) lists one 16 bit game, A on
    // "16 bit" keeps it listed and highlights that chip, A on "Tutte" moves the highlight back.
    s.tap(808, kUp);
    s.expectFocus(813, "search");
    s.type(814, "faro");
    s.expectVisible(815, 1, true);
    s.tap(816, kDown); // back into the filters, on the chip they remember (the last one)
    for (std::uint32_t tap = 0; tap < 4; ++tap) {
        s.tap(824 + 8 * tap, kLeft);
    }
    s.expectFocus(854, "chip-1");
    s.tap(856, kA);
    s.expectVisible(862, 1);
    s.expectClass(862, "chip-1", "active", true);
    s.expectClass(862, "chip-0", "active", false);
    s.tap(864, kLeft);
    s.tap(872, kA);
    s.expectClass(878, "chip-0", "active", true);
    s.expectClass(878, "chip-1", "active", false);
    s.tap(880, kUp);
    s.key(882, SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, 4);
    s.expectVisible(885, 20, true);
    s.tap(886, kDown);
    s.tap(894, kDown);
    s.expectFocus(900, "game-g03"); // the selected game: the search moved the selection to it
    s.expectHistory(900, 0);
    s.expectCard(900);

    // Text scaling (specification §12.4): x1.2 makes every text 1.2 times larger; back at x1 the layout is the first.
    s.layout(902, "text x1");
    s.textScale(904, 1.2f);
    s.layout(908, "text x1.2");
    s.largerText(908, "text x1.2", "text x1", 1.2f);
    s.textScale(910, 1.0f);
    s.layout(914, "text x1 again");
    s.sameLayout(914, "text x1 again", "text x1");
    // High contrast (specification §12.4): the theme's class on the document, opaque black panels; then the run's.
    s.highContrast(916, kOn);
    s.expectHighContrast(918, kOn);
    s.highContrast(920, kRunSetting);
    s.expectHighContrast(922, kRunSetting);

    // Measurements. Idle: no input. Taps: down and up taps 0.3 s apart between game-g03 and game-g04, each a resting
    // move (the card is laid out again in the same frame). Navigation: the D-pad held down, up, down for 100 frames each
    // (the focus moves every 3 to 6 frames, the card follows once it rests).
    for (std::uint32_t tap = 0; tap < 20; ++tap) {
        s.tap(1230 + 18 * tap, tap % 2 == 0 ? kDown : kUp);
    }
    s.expectSteady(1588, 1230, 20);
    s.expectFocus(1588, "game-g03");
    s.press(1590, kDown);
    s.release(1690, kDown);
    s.press(1690, kUp);
    s.release(1790, kUp);
    s.press(1790, kDown);
    s.release(1890, kDown);
    return s.finish(1910);
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
    case StepKind::TextScale:
        return "text-scale";
    case StepKind::HighContrast:
        return "high-contrast";
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
    case StepKind::ExpectCard:
        return "expect-card";
    case StepKind::ExpectInView:
        return "expect-in-view";
    case StepKind::ExpectClass:
        return "expect-class";
    case StepKind::ExpectOverflowCue:
        return "expect-overflow";
    case StepKind::ExpectHighContrast:
        return "expect-high-contrast";
    case StepKind::Layout:
        return "layout";
    case StepKind::SameLayout:
        return "same-layout";
    case StepKind::LargerText:
        return "larger-text";
    }
    return "unknown";
}

bool isCheck(StepKind kind) noexcept {
    switch (kind) {
    case StepKind::Press:
    case StepKind::Release:
    case StepKind::Stick:
    case StepKind::Resize:
    case StepKind::Text:
    case StepKind::Key:
    case StepKind::TextScale:
    case StepKind::HighContrast:
        return false;
    default:
        return true;
    }
}

} // namespace rv::spike_ui
