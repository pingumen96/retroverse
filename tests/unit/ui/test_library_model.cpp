// The library screen's model (I7 step 8, ADR-0018 §14): the sections and their focus, the removals the guard holds
// asked only after "Applica" is confirmed, a review row resolved only to one of its own candidates (those whose identity
// is present) after a confirmation, or kept separate; the quick-hash pass started, stopped and resumed; the content
// asked again when a refresh or a pass ends; the clicks press what the keys press; the watcher's modes on the roots.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <rv/ui/library_model.hpp>

using rv::ui::LibraryButton;
using rv::ui::LibraryClick;
using rv::ui::LibraryCommand;
using rv::ui::LibraryCommandKind;
using rv::ui::LibraryModel;
using rv::ui::LibraryTab;
using rv::ui::LibraryView;
using rv::ui::UiCommand;

namespace {

rv::ui::LibraryContent content() {
    rv::ui::LibraryContent made;
    made.roots = {{"esde-home", "", "C:/ES-DE", "reachable", "off"},
                  {"rom", "", "D:/roms", "unreachable", "off"},
                  {"media", "", "C:/media", "reachable", "off"}};
    made.held = {{"gb", "Pocket", 40, 50}, {"snes", "Lumen 16", 0, 0}};
    rv::ui::LibraryReview review;
    review.id = 7;
    review.gameId = "g0000000000000001";
    review.system = "Pocket";
    review.title = "Tiny Tennis";
    review.vanished = "A/Tiny Tennis (Europe).gb";
    review.candidates = {{"B/Tiny Tennis (Europe).gb", "g00000000000000b0"},
                         {"C/Tiny Tennis (Europe).gb", ""},
                         {"D/Tiny Tennis (Europe).gb", "g00000000000000d0"}};
    made.reviews = {review};
    made.files = 10;
    made.hashed = 4;
    return made;
}

/// Open, the content given, the request taken.
LibraryModel opened(rv::ui::LibraryLive live = {}) {
    LibraryModel model;
    live.canRefresh = true;
    model.setLive(live);
    model.open();
    REQUIRE(model.takeRequest() == LibraryCommand{LibraryCommandKind::Load, {}, 0, {}});
    model.setContent(content());
    return model;
}

void toTab(LibraryModel& model, LibraryTab tab) {
    while (model.tab() != tab) {
        REQUIRE_FALSE(model.apply(UiCommand::FilterNext).has_value());
    }
}

} // namespace

TEST_CASE("ui/library: opens on Origini, its button focused; the sections step and clamp", "[ui][library]") {
    LibraryModel model = opened();
    CHECK(model.isOpen());
    CHECK(model.state() == rv::ui::LibraryState::Listed);
    CHECK(model.tab() == LibraryTab::Sources);
    CHECK(model.focusedElement() == "lb-refresh");
    CHECK_FALSE(model.apply(UiCommand::Left).has_value());
    CHECK(model.tab() == LibraryTab::Sources);
    toTab(model, LibraryTab::Hashes);
    CHECK_FALSE(model.apply(UiCommand::Right).has_value());
    CHECK(model.tab() == LibraryTab::Hashes);
    CHECK(model.focusedElement() == "lb-hash");
    // Problemi has nothing to focus: the body scrolls.
    static_cast<void>(model.click(LibraryClick{LibraryClick::Kind::Tab, LibraryTab::Issues, LibraryButton::Refresh, 0}));
    CHECK(model.tab() == LibraryTab::Issues);
    CHECK(model.scrollsBody());
    CHECK(model.focusedElement().empty());
    CHECK(model.apply(UiCommand::Back) == LibraryCommand{LibraryCommandKind::Close, {}, 0, {}});
    CHECK_FALSE(model.isOpen());
}

TEST_CASE("ui/library: Aggiorna libreria asks a refresh, not while one runs", "[ui][library]") {
    LibraryModel model = opened();
    CHECK(model.apply(UiCommand::Activate) == LibraryCommand{LibraryCommandKind::Refresh, {}, 0, {}});
    rv::ui::LibraryLive running;
    running.canRefresh = true;
    running.refreshing = true;
    model.setLive(running);
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    // Its end asks the content again.
    rv::ui::LibraryLive ended;
    ended.canRefresh = true;
    ended.lastRefresh = "done";
    model.setLive(ended);
    CHECK(model.takeRequest() == LibraryCommand{LibraryCommandKind::Load, {}, 0, {}});
}

TEST_CASE("ui/library: the removals are held until Applica is confirmed", "[ui][library]") {
    LibraryModel model = opened();
    toTab(model, LibraryTab::Removals);
    CHECK(model.focusedElement() == "lb-apply-removals");
    // Applica le rimozioni asks first: nothing for the application, Annulla focused.
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == LibraryView::ConfirmRemovals);
    CHECK(model.focusedElement() == "lb-removals-cancel");
    // An A pressed twice by mistake cancels.
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == LibraryView::Main);
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == LibraryView::ConfirmRemovals);
    // Back leaves it too, nothing asked.
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.view() == LibraryView::Main);
    CHECK(model.isOpen());
    // Confirmed: the held systems, every one.
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.focusedElement() == "lb-apply");
    const std::optional<LibraryCommand> applied = model.apply(UiCommand::Activate);
    REQUIRE(applied.has_value());
    CHECK(applied->kind == LibraryCommandKind::ApplyRemovals);
    CHECK(applied->systems == std::vector<std::string>{"gb", "snes"});
    CHECK(model.view() == LibraryView::Main);
    // A click on Applica outside the confirmation presses nothing.
    CHECK_FALSE(model.click(LibraryClick{LibraryClick::Kind::Button, LibraryTab::Sources, LibraryButton::Apply, 0}).has_value());
    // No held system: no button.
    rv::ui::LibraryContent none = content();
    none.held.clear();
    model.setContent(none);
    CHECK(model.focusedElement().empty());
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK_FALSE(model.click(LibraryClick{LibraryClick::Kind::Button, LibraryTab::Sources, LibraryButton::ApplyRemovals, 0})
                    .has_value());
    CHECK(model.view() == LibraryView::Main);
}

TEST_CASE("ui/library: a review is resolved only to one of its candidates, after a confirmation", "[ui][library]") {
    LibraryModel model = opened();
    toTab(model, LibraryTab::Reviews);
    CHECK(model.focusedElement() == "lb-review-0");
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    REQUIRE(model.view() == LibraryView::Review);
    REQUIRE(model.review() != nullptr);
    CHECK(model.review()->id == 7);
    // The candidates whose identity is present, then Tieni separati, Annulla: C has none, and is skipped.
    CHECK(model.focusedElement() == "lb-candidate-0");
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.focusedElement() == "lb-candidate-2");
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.focusedElement() == "lb-keep-separate");
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.focusedElement() == "lb-review-cancel");
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.focusedElement() == "lb-review-cancel");
    // A click on the candidate without an identity does nothing.
    CHECK_FALSE(model.click(LibraryClick{LibraryClick::Kind::Candidate, LibraryTab::Sources, LibraryButton::Pair, 1}).has_value());
    CHECK(model.view() == LibraryView::Review);
    // Nor one past the candidates.
    CHECK_FALSE(model.click(LibraryClick{LibraryClick::Kind::Candidate, LibraryTab::Sources, LibraryButton::Pair, 9}).has_value());
    // D: asked first, Annulla focused; Annulla back to the review.
    CHECK_FALSE(model.apply(UiCommand::Up).has_value());
    CHECK_FALSE(model.apply(UiCommand::Up).has_value());
    CHECK(model.focusedElement() == "lb-candidate-2");
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    REQUIRE(model.view() == LibraryView::ConfirmPair);
    REQUIRE(model.candidate() != nullptr);
    CHECK(model.candidate()->path == "D/Tiny Tennis (Europe).gb");
    CHECK(model.focusedElement() == "lb-pair-cancel");
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == LibraryView::Review);
    // Abbina: its own candidate's GameId, and nothing else.
    CHECK_FALSE(model.click(LibraryClick{LibraryClick::Kind::Candidate, LibraryTab::Sources, LibraryButton::Pair, 0}).has_value());
    REQUIRE(model.view() == LibraryView::ConfirmPair);
    const std::optional<LibraryCommand> paired =
        model.click(LibraryClick{LibraryClick::Kind::Button, LibraryTab::Sources, LibraryButton::Pair, 0});
    REQUIRE(paired.has_value());
    CHECK(*paired == LibraryCommand{LibraryCommandKind::Resolve, {}, 7, "g00000000000000b0"});
    CHECK(model.resolving());
    // While it is answered, nothing else is asked.
    CHECK_FALSE(model.click(LibraryClick{LibraryClick::Kind::Button, LibraryTab::Sources, LibraryButton::KeepSeparate, 0}).has_value());
    model.resolved("conflict");
    CHECK_FALSE(model.resolving());
    CHECK(model.problem() == "conflict");
    CHECK(model.view() == LibraryView::Review);
    // Kept separate: no GameId.
    const std::optional<LibraryCommand> separate =
        model.click(LibraryClick{LibraryClick::Kind::Button, LibraryTab::Sources, LibraryButton::KeepSeparate, 0});
    REQUIRE(separate.has_value());
    CHECK(*separate == LibraryCommand{LibraryCommandKind::Resolve, {}, 7, {}});
    model.resolved("");
    CHECK(model.view() == LibraryView::Main);
    CHECK(model.tab() == LibraryTab::Reviews);
    CHECK(model.takeRequest() == LibraryCommand{LibraryCommandKind::Load, {}, 0, {}});
    // The row gone from the content: nothing stays on it.
    rv::ui::LibraryContent resolved = content();
    resolved.reviews.clear();
    model.setContent(resolved);
    CHECK(model.review() == nullptr);
    CHECK(model.focusedElement().empty());
}

TEST_CASE("ui/library: Calcola impronte starts, stops and resumes the pass", "[ui][library]") {
    LibraryModel model = opened();
    toTab(model, LibraryTab::Hashes);
    CHECK(model.apply(UiCommand::Activate) == LibraryCommand{LibraryCommandKind::StartHash, {}, 0, {}});
    rv::ui::LibraryLive live = model.live();
    live.hash = "running";
    live.hashTotal = 6;
    live.hashDone = 2;
    model.setLive(live);
    CHECK(model.apply(UiCommand::Activate) == LibraryCommand{LibraryCommandKind::CancelHash, {}, 0, {}});
    live.hash = "cancelling";
    model.setLive(live);
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value()); // being stopped: nothing more
    live.hash = "cancelled";
    live.hashRemaining = 4;
    model.setLive(live);
    // Its end asks the counts again; Riprendi starts it from what is left.
    CHECK(model.takeRequest() == LibraryCommand{LibraryCommandKind::Load, {}, 0, {}});
    CHECK(model.apply(UiCommand::Activate) == LibraryCommand{LibraryCommandKind::StartHash, {}, 0, {}});
}

TEST_CASE("ui/library: the roots take the watcher's modes", "[ui][library]") {
    rv::ui::LibraryLive live;
    const std::vector<rv::ui::LibraryRoot> roots = {{"esde-home", "", "", "reachable", "off"},
                                                    {"rom", "", "", "reachable", "off"},
                                                    {"system", "psx", "", "reachable", "off"},
                                                    {"media", "", "", "reachable", "off"}};
    for (const rv::ui::LibraryRoot& root : rv::ui::withModes(roots, live)) {
        CHECK(root.mode == "off");
    }
    live.watching = true;
    live.modes = {{"rom", "notify"},      {"system:psx", "poll"}, {"media", "lost"},
                  {"gamelists", "notify"}, {"settings", "poll"},   {"custom-systems", "notify"}};
    const std::vector<rv::ui::LibraryRoot> shown = rv::ui::withModes(roots, live);
    CHECK(shown[0].mode == "poll"); // the worst of the ES-DE home's three
    CHECK(shown[1].mode == "notify");
    CHECK(shown[2].mode == "poll");
    CHECK(shown[3].mode == "lost");
}
