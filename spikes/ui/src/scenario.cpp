#include "scenario.hpp"

#include <algorithm>
#include <utility>

namespace rv::spike_ui {
namespace {

/// Frames a scripted tap holds its button: well under the 0.4 s repeat delay, so one step per tap.
constexpr std::uint32_t kTapFrames = 3;
/// Frames between two taps of a row of taps, and from an input to the check of what it did.
constexpr std::uint32_t kTapSpacing = 8;
constexpr std::uint32_t kSettle = 6;
/// The ids of the first positions of the list the facts keep.
constexpr std::size_t kFactIds = 64;

/// Builds the step list in any order; finish() sorts it by frame (stable: steps of one frame keep their order).
class ScenarioBuilder {
public:
    void press(std::uint32_t frame, SDL_GamepadButton button) { add(frame, StepKind::Press).button = button; }
    void release(std::uint32_t frame, SDL_GamepadButton button) { add(frame, StepKind::Release).button = button; }
    void tap(std::uint32_t frame, SDL_GamepadButton button) {
        press(frame, button);
        release(frame + kTapFrames, button);
    }
    /// Taps `button` `times` times kTapSpacing frames apart from `frame`; returns the frame after the last tap's check
    /// delay.
    std::uint32_t taps(std::uint32_t frame, SDL_GamepadButton button, std::size_t times) {
        for (std::size_t tap = 0; tap < times; ++tap) {
            this->tap(frame + static_cast<std::uint32_t>(tap) * kTapSpacing, button);
        }
        return frame + static_cast<std::uint32_t>(times > 0 ? times - 1 : 0) * kTapSpacing + kSettle;
    }
    void stick(std::uint32_t frame, float x, float y) { add(frame, StepKind::Stick).value = glm::vec2(x, y); }
    void trigger(std::uint32_t frame, SDL_GamepadAxis axis, float value) {
        Step& step = add(frame, StepKind::Trigger);
        step.axis = axis;
        step.value = glm::vec2(value);
    }
    void triggerTap(std::uint32_t frame, SDL_GamepadAxis axis) {
        trigger(frame, axis, 1.0f);
        trigger(frame + kTapFrames, axis, 0.0f);
    }
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
    void backspace(std::uint32_t frame, std::size_t times) {
        key(frame, SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, static_cast<std::uint32_t>(times));
    }
    void textScale(std::uint32_t frame, float factor) { add(frame, StepKind::TextScale).value = glm::vec2(factor); }
    /// 1 on, 0 off, 2 the run's setting.
    void highContrast(std::uint32_t frame, std::uint32_t state) { add(frame, StepKind::HighContrast).count = state; }
    void mark(std::uint32_t frame, std::string label) { add(frame, StepKind::Mark).label = std::move(label); }
    void expectFocus(std::uint32_t frame, std::string element) {
        add(frame, StepKind::ExpectFocus).text = std::move(element);
    }
    void expectSteady(std::uint32_t frame, std::uint32_t since, std::uint32_t moves) {
        Step& step = add(frame, StepKind::ExpectSteady);
        step.since = since;
        step.count = moves;
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
    void expectCount(std::uint32_t frame, std::size_t games, bool keyboard = false) {
        Step& step = add(frame, StepKind::ExpectCount);
        step.position = games;
        step.keyboard = keyboard;
    }
    void expectSelected(std::uint32_t frame, std::size_t position, bool keyboard = false, std::string id = {}) {
        Step& step = add(frame, StepKind::ExpectSelected);
        step.position = position;
        step.keyboard = keyboard;
        step.text = std::move(id);
    }
    void expectPages(std::uint32_t frame, std::string label, int pages) {
        Step& step = add(frame, StepKind::ExpectPages);
        step.label = std::move(label);
        step.pages = pages;
    }
    void expectRow(std::uint32_t frame) { add(frame, StepKind::ExpectRow); }
    void expectWindow(std::uint32_t frame) { add(frame, StepKind::ExpectWindow); }
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

[[nodiscard]] std::string chip(std::size_t index) {
    return "chip-" + std::to_string(index);
}

/// The first word of `title` made of four or more ASCII letters, in lowercase (typed in the search field, where every
/// character is one Backspace).
[[nodiscard]] std::string asciiWordOf(std::string_view title) {
    std::string word;
    for (std::size_t at = 0; at <= title.size(); ++at) {
        const char c = at < title.size() ? title[at] : ' ';
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool boundary = c == ' ' || c == '\'';
        if (letter) {
            word.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            continue;
        }
        if (boundary && word.size() >= 4) {
            return word;
        }
        word.clear();
        if (!boundary) {
            // A letter outside ASCII: skip the rest of this word.
            while (at + 1 < title.size() && title[at + 1] != ' ') {
                ++at;
            }
        }
    }
    return {};
}

[[nodiscard]] tl::unexpected<Error> factError(std::string message) {
    return makeError(ErrorCode::ValidationFailed, std::move(message), "spike_ui scenario facts");
}

} // namespace

std::string_view toString(ScenarioKind kind) noexcept {
    switch (kind) {
    case ScenarioKind::QuickCatalog:
        return "quick-catalog";
    case ScenarioKind::UserBindings:
        return "user-bindings";
    }
    return "unknown";
}

std::optional<ScenarioKind> scenarioKindFromName(std::string_view name) noexcept {
    for (const ScenarioKind kind : {ScenarioKind::QuickCatalog, ScenarioKind::UserBindings}) {
        if (toString(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

Expected<ScenarioFacts> readScenarioFacts(ui::CatalogSource& source, std::string_view markerWord) {
    ScenarioFacts facts;
    facts.markerSearch = std::string(markerWord);
    std::vector<ui::CatalogEntry> rows;
    const auto run = [&source](std::string text, std::string platform) {
        return source.query(ui::CatalogSourceQuery{std::move(text), std::move(platform)});
    };

    if (Status listed = run({}, {}); !listed) {
        return propagate(listed.error());
    }
    facts.count = source.count();
    if (Status read = source.rows(0, kFactIds, rows); !read) {
        return propagate(read.error());
    }
    for (const ui::CatalogEntry& row : rows) {
        facts.ids.push_back(row.id);
    }
    Expected<std::vector<ui::PlatformFilter>> platforms = source.platforms();
    if (!platforms) {
        return propagate(platforms.error());
    }
    facts.chips = platforms->size() + 1;
    if (facts.count < kFactIds || platforms->size() < 2) {
        return factError("the scenario needs 64 games or more on two platforms or more");
    }

    // The marker: the only game its word finds, on the first platform.
    if (Status found = run(std::string(markerWord), {}); !found) {
        return propagate(found.error());
    }
    if (source.count() != 1 || !source.rows(0, 1, rows) || rows.size() != 1) {
        return factError("the marker word must find exactly one game");
    }
    const ui::CatalogEntry marker = rows.front();
    facts.markerId = marker.id;
    facts.markerWord = asciiWordOf(marker.title);
    if (facts.markerWord.empty()) {
        return factError("the marker's title needs a word of four ASCII letters or more");
    }
    const auto onPlatform =
        std::find_if(platforms->begin(), platforms->end(),
                     [&marker](const ui::PlatformFilter& filter) { return filter.key == marker.platform; });
    if (onPlatform != platforms->begin()) {
        return factError("the marker must be on the first platform (the first platform chip)");
    }
    facts.markerChip = 1;
    facts.markerChipCount = onPlatform->count;

    const auto positionOf = [&source](const std::string& id) {
        const std::optional<std::size_t> position = source.position(id);
        return position.value_or(static_cast<std::size_t>(-1));
    };
    if (Status listed = run({}, {}); !listed) {
        return propagate(listed.error());
    }
    facts.marker = positionOf(facts.markerId);
    if (Status listed = run({}, marker.platform); !listed) {
        return propagate(listed.error());
    }
    facts.markerInChip = positionOf(facts.markerId);
    if (Status listed = run(facts.markerWord, {}); !listed) {
        return propagate(listed.error());
    }
    facts.markerWordCount = source.count();
    facts.markerInWord = positionOf(facts.markerId);

    // The second platform (two games or more: the scenario steps down to the second), its first game, and where the
    // full list has it.
    const ui::PlatformFilter& second = (*platforms)[1];
    if (Status listed = run({}, second.key); !listed) {
        return propagate(listed.error());
    }
    facts.secondChipCount = source.count();
    if (facts.secondChipCount < 2 || !source.rows(0, 1, rows) || rows.size() != 1) {
        return factError("the second platform needs two games or more");
    }
    const std::string firstId = rows.front().id;
    if (Status listed = run({}, {}); !listed) {
        return propagate(listed.error());
    }
    facts.secondChipFirstInAll = positionOf(firstId);
    if (facts.marker >= facts.count || facts.markerInChip >= facts.markerChipCount ||
        facts.markerInWord >= facts.markerWordCount || facts.secondChipFirstInAll >= facts.count) {
        return factError("a game of the scenario is missing from a list");
    }
    return facts;
}

Scenario defaultScenario(const ScenarioFacts& facts) {
    constexpr SDL_GamepadButton kDown = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    constexpr SDL_GamepadButton kUp = SDL_GAMEPAD_BUTTON_DPAD_UP;
    constexpr SDL_GamepadButton kLeft = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    constexpr SDL_GamepadButton kRight = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    constexpr SDL_GamepadButton kA = SDL_GAMEPAD_BUTTON_SOUTH;
    constexpr SDL_GamepadButton kB = SDL_GAMEPAD_BUTTON_EAST;
    constexpr SDL_GamepadButton kX = SDL_GAMEPAD_BUTTON_WEST;
    constexpr SDL_GamepadButton kY = SDL_GAMEPAD_BUTTON_NORTH;
    constexpr SDL_GamepadButton kHome = SDL_GAMEPAD_BUTTON_LEFT_STICK;
    constexpr SDL_GamepadButton kEnd = SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    constexpr SDL_GamepadButton kPreviousFilter = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    constexpr SDL_GamepadButton kNextFilter = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    constexpr SDL_GamepadAxis kPageUp = SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    constexpr SDL_GamepadAxis kPageDown = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    constexpr std::uint32_t kOn = 1;
    constexpr std::uint32_t kRunSetting = 2;
    const std::string& marker = facts.markerSearch;
    const std::size_t lastChip = facts.chips - 1;

    ScenarioBuilder s;
    // The task's sequence: the selection down 10 times (the first game has the focus at start), right into the card's
    // actions, activate (Avvia), back (to the game the actions were opened from).
    s.taps(30, kDown, 10);
    s.expectSelected(108, 10);
    s.expectRow(108);
    s.tap(110, kRight);
    // The downs came 8 frames apart, faster than the card follows (0.15 s): entering the actions fills it at once, so
    // Avvia shows the game it launches (the focus moves at frame 111; the card would catch up at 113 by itself).
    s.expectCard(112);
    s.expectFocus(116, "act-play");
    s.expectHistory(116, 1);
    s.tap(120, kA);
    s.expectLaunched(126, facts.ids[10]);
    s.tap(130, kB);
    s.expectRow(136);
    s.expectSelected(136, 10);
    s.expectHistory(136, 0);

    // The secondary actions: Y "Vedi media", X "Informazioni" twice (shown, hidden).
    s.tap(140, kY);
    s.expectMedia(146, facts.ids[10]);
    s.tap(150, kX);
    s.expectInfo(156, true);
    s.tap(160, kX);
    s.expectInfo(166, false);

    // The left stick held down for one second: one step at once, then one per 0.1 s after 0.4 s (7 steps); the window
    // scrolls once the selection reaches its bottom margin.
    s.stick(170, 0.0f, -0.9f);
    s.stick(230, 0.0f, 0.0f);
    s.expectSelected(236, 17);
    s.expectRow(236);
    s.expectWindow(236);
    // A stick resting just under its engage threshold (a worn stick, a thumb on it): 0.57 and 0.61 of its travel are
    // 0.49 and 0.54 past the bindings' 0.15 dead zone, under 0.55. No step.
    for (std::uint32_t frame = 240; frame < 360; ++frame) {
        s.stick(frame, 0.01f, frame % 2 == 0 ? -0.57f : -0.61f);
    }
    s.stick(360, 0.0f, 0.0f);
    s.expectSteady(364, 241, 0);
    s.expectSelected(364, 17);
    // A stick near the diagonal: one step down, no turn to the right (it would leave the list for the actions).
    s.stick(370, 0.6f, -0.62f);
    s.stick(385, 0.66f, -0.6f);
    s.stick(392, 0.0f, 0.0f);
    s.expectSteady(398, 371, 1);
    s.expectSelected(398, 18);

    // Up held: the selection repeats faster after 1.5 s, reaches the first game, crosses into the filters, then the
    // search field.
    s.press(400, kUp);
    s.release(560, kUp);
    s.expectFocus(565, "search");
    s.expectHistory(565, 2);
    // The keyboard types into the focused search field (the list follows as it changes): the marker alone, the first
    // game of that list. Erased, the whole list comes back with the marker still selected: its window is in the middle
    // of the 100,000 games.
    s.type(566, marker);
    s.expectCount(567, 1, true);
    s.expectSelected(567, 0, true, facts.markerId);
    s.backspace(568, marker.size());
    s.expectCount(570, facts.count, true);
    s.expectSelected(570, facts.marker, true, facts.markerId);
    s.expectWindow(570);
    // A word of the marker's title narrows the list to the games it finds; the marker stays selected, at its place in
    // the narrower list, and erased again in the whole one.
    s.type(572, facts.markerWord);
    s.expectCount(573, facts.markerWordCount, true);
    s.expectSelected(573, facts.markerInWord, true, facts.markerId);
    s.expectWindow(573);
    s.backspace(574, facts.markerWord.size());
    s.expectCount(576, facts.count, true);
    s.expectSelected(576, facts.marker, true, facts.markerId);

    // Down: back into the filters (Back would now return to the list).
    s.tap(578, kDown);
    s.expectFocus(584, chip(0));
    s.expectHistory(584, 1);
    // Two filters to the right, A filters the list (the second platform: the marker is not on it, the first game of
    // the new list is selected), down lands on it.
    s.tap(586, kRight);
    s.tap(594, kRight);
    s.expectFocus(600, chip(2));
    s.tap(602, kA);
    s.expectCount(608, facts.secondChipCount);
    s.expectClass(608, chip(2), "active", true);
    s.tap(610, kDown);
    s.expectSelected(616, 0);
    s.expectRow(616);
    s.expectHistory(616, 0);
    s.tap(618, kDown);
    s.expectSelected(624, 1);
    // Back up to the filters: the first Up selects the platform's first game again, the second leaves the list for
    // the filters (they remember the second platform); left to "Tutte", A, down into the list: the game selected under
    // the filter, at its place in the whole list.
    s.tap(626, kUp);
    s.tap(634, kUp);
    s.expectFocus(640, chip(2));
    s.expectSelected(640, 0);
    s.tap(642, kLeft);
    s.tap(650, kLeft);
    s.expectFocus(656, chip(0));
    s.tap(658, kA);
    s.expectCount(664, facts.count);
    s.expectSelected(664, facts.secondChipFirstInAll);
    s.tap(666, kDown);
    s.expectSelected(672, facts.secondChipFirstInAll);
    s.expectRow(672);

    // Home (the left stick's click): the first game, the window at the top. Then the resize: 1280x720 (the compact
    // layout shows fewer rows), then back to 1920x1080, whose layout must equal the one before.
    s.tap(674, kHome);
    s.expectSelected(680, 0);
    s.expectRow(680);
    s.layout(682, "1920x1080");
    s.resize(684, 1280, 720);
    s.layout(696, "1280x720");
    s.expectWindow(696);
    s.resize(698, 1920, 1080);
    s.layout(710, "1920x1080 again");
    s.sameLayout(710, "1920x1080 again", "1920x1080");

    // A stick crossing its engage threshold every frame, 0.66 and 0.60 of its travel (0.60 and 0.53 past the dead
    // zone): the hysteresis holds the direction, so it steps at the held cadence (at once, again after 0.4 s), never on
    // every crossing (a single threshold would step every other frame).
    for (std::uint32_t frame = 714; frame < 742; ++frame) {
        s.stick(frame, 0.01f, frame % 2 == 0 ? -0.66f : -0.60f);
    }
    s.stick(742, 0.0f, 0.0f);
    s.expectSteady(748, 714, 2);
    s.expectSelected(748, 2);

    // Up to the first game and into the filters (they remember "Tutte"), then right to the last chip: the row scrolls
    // it into view, and its fades mark the edges chips hide beyond (the end of the row first, its start once scrolled).
    std::uint32_t at = s.taps(750, kUp, 3);
    s.expectFocus(at, chip(0));
    s.expectOverflowCue(at);
    at = s.taps(at + 2, kRight, lastChip);
    s.expectFocus(at, chip(lastChip));
    s.expectInView(at, chip(lastChip), "chips");
    s.expectOverflowCue(at);

    // A filter that lets the same games through still moves the highlight: the marker alone (typed), A on its
    // platform's chip keeps it listed and highlights that chip, A on "Tutte" moves the highlight back. The marker's word
    // is typed in two halves, the second before the first: Home is a command key, which the screen sends to the search
    // field as RmlUi's Home, so its caret goes to the start (else the word would read the halves the other way round).
    const std::size_t half = marker.size() / 2;
    s.tap(at + 2, kUp);
    s.expectFocus(at + 7, "search");
    s.type(at + 8, marker.substr(half));
    s.key(at + 9, SDL_SCANCODE_HOME, SDLK_HOME, 1);
    s.type(at + 11, marker.substr(0, half));
    s.expectCount(at + 12, 1, true);
    s.tap(at + 14, kDown); // back into the filters, on the chip they remember (the last one)
    at = s.taps(at + 22, kLeft, lastChip - facts.markerChip);
    s.expectFocus(at, chip(facts.markerChip));
    s.tap(at + 2, kA);
    s.expectCount(at + 8, 1);
    s.expectClass(at + 8, chip(facts.markerChip), "active", true);
    s.expectClass(at + 8, chip(0), "active", false);
    at = s.taps(at + 10, kLeft, facts.markerChip);
    s.tap(at, kA);
    s.expectClass(at + 6, chip(0), "active", true);
    s.expectClass(at + 6, chip(facts.markerChip), "active", false);
    // Back in the search field End puts the caret after the word (it was in the middle), Backspace erases it.
    s.tap(at + 8, kUp);
    s.key(at + 10, SDL_SCANCODE_END, SDLK_END, 1);
    s.backspace(at + 12, marker.size());
    s.expectCount(at + 15, facts.count, true);
    s.tap(at + 16, kDown);
    s.tap(at + 24, kDown);
    at += 30;
    s.expectSelected(at, facts.marker, false, facts.markerId); // in the middle: the search moved the selection to it
    s.expectRow(at);
    s.expectWindow(at);
    s.expectHistory(at, 0);
    s.expectCard(at);

    // Text scaling (specification §12.4): x1.2 makes every text 1.2 times larger (the window shows fewer rows); back at
    // x1 the layout is the first. The selection sits on the window's top margin row, where both changes keep it.
    s.layout(at + 2, "text x1");
    s.textScale(at + 4, 1.2f);
    s.layout(at + 8, "text x1.2");
    s.largerText(at + 8, "text x1.2", "text x1", 1.2f);
    s.expectWindow(at + 8);
    s.textScale(at + 10, 1.0f);
    s.layout(at + 14, "text x1 again");
    s.sameLayout(at + 14, "text x1 again", "text x1");
    // High contrast (specification §12.4): the theme's class on the document, opaque black panels; then the run's.
    s.highContrast(at + 16, kOn);
    s.expectHighContrast(at + 18, kOn);
    s.highContrast(at + 20, kRunSetting);
    s.expectHighContrast(at + 22, kRunSetting);
    at += 24;

    // The shoulders change the filter from the list: the next one (the marker's platform) keeps the marker, at its
    // place in that list; the previous one ("Tutte") brings it back to the middle. Twice the next: the second
    // platform does not have the marker, its first game is selected; twice the previous: that game's place in the whole
    // list, the first game.
    s.tap(at, kNextFilter);
    s.expectCount(at + kSettle, facts.markerChipCount);
    s.expectClass(at + kSettle, chip(facts.markerChip), "active", true);
    s.expectSelected(at + kSettle, facts.markerInChip, false, facts.markerId);
    s.expectRow(at + kSettle);
    s.expectWindow(at + kSettle);
    s.tap(at + 8, kPreviousFilter);
    s.expectCount(at + 8 + kSettle, facts.count);
    s.expectClass(at + 8 + kSettle, chip(0), "active", true);
    s.expectSelected(at + 8 + kSettle, facts.marker, false, facts.markerId);
    at = s.taps(at + 16, kNextFilter, 2);
    s.expectCount(at, facts.secondChipCount);
    s.expectClass(at, chip(2), "active", true);
    s.expectSelected(at, 0);
    s.expectRow(at);
    at = s.taps(at + 2, kPreviousFilter, 2);
    s.expectClass(at, chip(0), "active", true);
    s.expectSelected(at, 0);

    // End (the right stick's click): the last game, the window at the end of the list.
    s.tap(at + 2, kEnd);
    at += 2 + kSettle;
    s.expectSelected(at, facts.count - 1);
    s.expectRow(at);
    s.expectWindow(at);
    s.mark(at, "end");
    // The triggers page: up a page from the end, down again to the last game.
    s.triggerTap(at + 2, kPageUp);
    s.expectPages(at + 2 + kSettle, "end", -1);
    s.expectRow(at + 2 + kSettle);
    s.expectWindow(at + 2 + kSettle);
    s.triggerTap(at + 10, kPageDown);
    s.expectSelected(at + 10 + kSettle, facts.count - 1);
    at += 10 + kSettle;
    // Home, then the right trigger held for 0.55 s: a page at once, again after 0.4 s and 0.5 s.
    s.tap(at + 2, kHome);
    s.expectSelected(at + 2 + kSettle, 0);
    s.mark(at + 2 + kSettle, "home");
    at += 10;
    s.trigger(at, kPageDown, 1.0f);
    s.trigger(at + 33, kPageDown, 0.0f);
    at += 33 + kSettle;
    s.expectPages(at, "home", 3);
    s.expectRow(at);
    s.expectWindow(at);
    // Two games down: the selection off the window's top margin, where the measured taps neither scroll the window.
    at = s.taps(at + 2, kDown, 2);
    s.mark(at, "taps");

    // Measurements. Idle: no input. Taps: down and up taps 0.3 s apart, each a resting move (the card is laid out again
    // in the same frame). Navigation: the D-pad held down, up, down for 100 frames each (the selection moves every 3 to
    // 6 frames and the window scrolls with it, the card follows once it rests).
    const std::uint32_t idle = at + 4;
    const std::uint32_t taps = idle + 300;
    const std::uint32_t navigation = taps + 360;
    const std::uint32_t end = navigation + 300;
    s.phase("startup", 0, 30);
    s.phase("sequence", 30, idle);
    s.phase(kIdlePhase, idle, taps);
    s.phase(kTapsPhase, taps, navigation);
    s.phase(kNavigationPhase, navigation, end);
    for (std::uint32_t tap = 0; tap < 20; ++tap) {
        s.tap(taps + 18 * tap, tap % 2 == 0 ? kDown : kUp);
    }
    s.expectSteady(navigation - 2, taps, 20);
    s.expectPages(navigation - 2, "taps", 0);
    s.press(navigation, kDown);
    s.release(navigation + 100, kDown);
    s.press(navigation + 100, kUp);
    s.release(navigation + 200, kUp);
    s.press(navigation + 200, kDown);
    s.release(end, kDown);
    return s.finish(end + 20);
}

Scenario userBindingsScenario() {
    ScenarioBuilder s;
    s.phase("startup", 0, 30);
    // The user's bindings.json binds ui_down to X alone: the D-pad down moves nothing.
    s.tap(30, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    s.expectSelected(36, 0);
    // X moves the selection down, twice, and no longer shows the card's details (ui_info unbound).
    s.tap(40, SDL_GAMEPAD_BUTTON_WEST);
    s.expectSelected(46, 1);
    s.expectRow(46);
    s.tap(50, SDL_GAMEPAD_BUTTON_WEST);
    s.expectSelected(56, 2);
    s.expectInfo(56, false);
    // The actions the file leaves out keep their default bindings: the D-pad up.
    s.tap(60, SDL_GAMEPAD_BUTTON_DPAD_UP);
    s.expectSelected(66, 1);
    s.expectRow(66);
    return s.finish(80);
}

std::string_view toString(StepKind kind) noexcept {
    switch (kind) {
    case StepKind::Press:
        return "press";
    case StepKind::Release:
        return "release";
    case StepKind::Stick:
        return "stick";
    case StepKind::Trigger:
        return "trigger";
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
    case StepKind::Mark:
        return "mark";
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
    case StepKind::ExpectCount:
        return "expect-count";
    case StepKind::ExpectSelected:
        return "expect-selected";
    case StepKind::ExpectPages:
        return "expect-pages";
    case StepKind::ExpectRow:
        return "expect-row";
    case StepKind::ExpectWindow:
        return "expect-window";
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
    case StepKind::Trigger:
    case StepKind::Resize:
    case StepKind::Text:
    case StepKind::Key:
    case StepKind::TextScale:
    case StepKind::HighContrast:
    case StepKind::Mark:
        return false;
    default:
        return true;
    }
}

} // namespace rv::spike_ui
