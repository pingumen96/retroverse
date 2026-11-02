// The emulators screen's model (I7 step 4, ADR-0018 §5): the systems, a system's options, a choice waiting for its test,
// what the application is asked, each by the keys and by clicks that press what the keys press.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <rv/ui/emulators_model.hpp>

using namespace rv::ui;

namespace {

EmulatorOption option(std::string name, std::string readiness = "ready", bool chosen = false) {
    EmulatorOption made;
    made.value = name;
    made.name = std::move(name);
    made.readiness = std::move(readiness);
    made.chosen = chosen;
    return made;
}

std::vector<EmulatorSystem> table() {
    EmulatorSystem gb;
    gb.name = "gb";
    gb.fullName = "Pocket";
    gb.games = 3;
    gb.source = "first-command";
    gb.choice = "Fake";
    gb.options = {option("Fake", "ready", true), option("Home", "ready"), option("Gone", "emulator-not-found")};
    gb.options[2].emulator = "GONE";
    EmulatorSystem psx;
    psx.name = "psx";
    psx.fullName = "Disc";
    psx.games = 1;
    psx.readiness = "requires-configuration";
    psx.blocker = "token-unsupported";
    psx.options = {option("Script", "requires-configuration", true)};
    return {gb, psx};
}

std::vector<EmulatorSample> samples() {
    return {EmulatorSample{"g1", "First"}, EmulatorSample{"g2", "Second"}};
}

/// Open, the table answered.
EmulatorsModel listed() {
    EmulatorsModel model;
    model.open();
    CHECK(model.takeRequest()->kind == EmulatorsCommandKind::Load);
    model.setTable(table());
    return model;
}

/// On the option `index` of gb, its samples answered.
EmulatorsModel onChoice(std::size_t index = 0) {
    EmulatorsModel model = listed();
    const auto asked = model.apply(UiCommand::Activate);
    REQUIRE(asked.has_value());
    CHECK(asked->kind == EmulatorsCommandKind::Samples);
    CHECK(asked->system == "gb");
    model.setSamples("gb", samples());
    for (std::size_t step = 0; step < index; ++step) {
        CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    }
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == EmulatorsView::Choice);
    return model;
}

} // namespace

TEST_CASE("ui/emulators: the table: loading, its counts, a catalog without one", "[ui][emulators]") {
    EmulatorsModel model;
    CHECK_FALSE(model.isOpen());
    model.open();
    CHECK(model.isOpen());
    CHECK(model.state() == EmulatorsState::Loading);
    CHECK(model.takeRequest()->kind == EmulatorsCommandKind::Load);
    CHECK_FALSE(model.takeRequest().has_value());
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value()); // nothing to open yet
    model.setTable(table());
    CHECK(model.state() == EmulatorsState::Listed);
    CHECK(model.readySystems() == 1);
    CHECK(model.blockedSystems() == 1);
    CHECK(model.focusedElement() == "em-system-0");
    model.setUnavailable();
    CHECK(model.state() == EmulatorsState::Unavailable);
    CHECK(model.systems().empty());
    const auto closed = model.apply(UiCommand::Back);
    REQUIRE(closed.has_value());
    CHECK(closed->kind == EmulatorsCommandKind::Close);
    CHECK_FALSE(model.isOpen());
}

TEST_CASE("ui/emulators: the rows move and stop at their ends, Back walks out one level", "[ui][emulators]") {
    EmulatorsModel model = listed();
    CHECK_FALSE(model.apply(UiCommand::Up).has_value());
    CHECK(model.focusedSystem() == 0);
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.focusedSystem() == 1);
    CHECK(model.focusedElement() == "em-system-1");
    CHECK_FALSE(model.apply(UiCommand::Home).has_value());
    CHECK(model.focusedSystem() == 0);
    CHECK_FALSE(model.apply(UiCommand::End).has_value());
    CHECK(model.focusedSystem() == 1);
    CHECK_FALSE(model.apply(UiCommand::PageUp).has_value());
    CHECK(model.focusedSystem() == 0);

    EmulatorsModel choosing = onChoice();
    CHECK_FALSE(choosing.apply(UiCommand::Back).has_value());
    CHECK(choosing.view() == EmulatorsView::System);
    CHECK(choosing.option() == nullptr);
    CHECK_FALSE(choosing.apply(UiCommand::Back).has_value());
    CHECK(choosing.view() == EmulatorsView::Systems);
    CHECK(choosing.apply(UiCommand::Back)->kind == EmulatorsCommandKind::Close);
}

TEST_CASE("ui/emulators: a ready choice focuses its test; the sample steps round", "[ui][emulators]") {
    EmulatorsModel model = onChoice(1);
    REQUIRE(model.option() != nullptr);
    CHECK(model.option()->value == "Home");
    CHECK(model.buttons() == std::vector<EmulatorsButton>{EmulatorsButton::Sample, EmulatorsButton::Test,
                                                          EmulatorsButton::Save, EmulatorsButton::Cancel});
    CHECK(model.focusedButton() == EmulatorsButton::Test);
    CHECK(model.focusedElement() == "em-test");

    CHECK_FALSE(model.apply(UiCommand::Up).has_value());
    CHECK(model.focusedButton() == EmulatorsButton::Sample);
    CHECK_FALSE(model.apply(UiCommand::Right).has_value());
    CHECK(model.sample() == 1);
    CHECK_FALSE(model.apply(UiCommand::Right).has_value());
    CHECK(model.sample() == 0);
    CHECK_FALSE(model.apply(UiCommand::Left).has_value());
    CHECK(model.sample() == 1);
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());

    const auto test = model.apply(UiCommand::Activate);
    REQUIRE(test.has_value());
    CHECK(test->kind == EmulatorsCommandKind::Test);
    CHECK(test->system == "gb");
    CHECK(test->value == "Home");
    CHECK_FALSE(test->profile);
    CHECK(test->game == "g2");
    CHECK(test->gameTitle == "Second");
}

TEST_CASE("ui/emulators: a test holds the view; a clean end goes back, another stays and says so", "[ui][emulators]") {
    EmulatorsModel model = onChoice(1);
    model.testStarted();
    CHECK(model.view() == EmulatorsView::Testing);
    CHECK(model.verdict() == "running");
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == EmulatorsView::Testing);

    model.testEnded("crashed");
    CHECK(model.view() == EmulatorsView::Choice);
    CHECK(model.verdict() == "crashed");
    REQUIRE(model.option() != nullptr);
    CHECK(model.option()->value == "Home"); // the choice stays pending, the test can run again
    CHECK_FALSE(model.takeRequest().has_value());

    model.testStarted();
    model.testEnded("verified");
    CHECK(model.view() == EmulatorsView::System);
    CHECK(model.verdict() == "verified");
    CHECK(model.option() == nullptr);
    CHECK(model.takeRequest()->kind == EmulatorsCommandKind::Load); // the table says so now
}

TEST_CASE("ui/emulators: an option that cannot be tried: its repair first, Salva senza provare", "[ui][emulators]") {
    EmulatorsModel model = onChoice(2);
    CHECK(model.option()->readiness == "emulator-not-found");
    CHECK(model.buttons() ==
          std::vector<EmulatorsButton>{EmulatorsButton::Program, EmulatorsButton::Save, EmulatorsButton::Cancel});
    CHECK(model.focusedButton() == EmulatorsButton::Program);
    const auto browse = model.apply(UiCommand::Activate);
    REQUIRE(browse.has_value());
    CHECK(browse->kind == EmulatorsCommandKind::Browse);
    CHECK(browse->emulator == "GONE");

    // The program found: the same choice, now ready, can be tried.
    model.changeEnded({}, false);
    CHECK(model.takeRequest()->kind == EmulatorsCommandKind::Load);
    std::vector<EmulatorSystem> repaired = table();
    repaired[0].options[2].readiness = "ready";
    repaired[0].options[2].programChosen = true;
    model.setTable(std::move(repaired));
    CHECK(model.view() == EmulatorsView::Choice);
    CHECK(model.buttons() ==
          std::vector<EmulatorsButton>{EmulatorsButton::Sample, EmulatorsButton::Test, EmulatorsButton::ClearProgram,
                                       EmulatorsButton::Save, EmulatorsButton::Cancel});

    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    while (model.focusedButton() != EmulatorsButton::Save) {
        CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    }
    const auto save = model.apply(UiCommand::Activate);
    REQUIRE(save.has_value());
    CHECK(save->kind == EmulatorsCommandKind::Save);
    CHECK(save->value == "Gone");
    model.changeEnded({}, true);
    CHECK(model.view() == EmulatorsView::System);
    CHECK(model.savedUntested());
}

TEST_CASE("ui/emulators: a change that fails stays on the choice and says why", "[ui][emulators]") {
    EmulatorsModel model = onChoice(2);
    model.changeEnded("refused", false);
    CHECK(model.view() == EmulatorsView::Choice);
    CHECK(model.problem() == "refused");
    CHECK_FALSE(model.takeRequest().has_value());
}

TEST_CASE("ui/emulators: a stored choice can go back to ES-DE's", "[ui][emulators]") {
    EmulatorsModel model;
    model.open();
    std::vector<EmulatorSystem> rows = table();
    rows[0].stored = true;
    model.setTable(std::move(rows));
    static_cast<void>(model.apply(UiCommand::Activate));
    CHECK(model.optionRows() == 4);
    CHECK(model.offersClear());
    CHECK_FALSE(model.apply(UiCommand::End).has_value());
    CHECK(model.focusedElement() == "em-option-3");
    const auto clear = model.apply(UiCommand::Activate);
    REQUIRE(clear.has_value());
    CHECK(clear->kind == EmulatorsCommandKind::Clear);
    CHECK(clear->system == "gb");
}

TEST_CASE("ui/emulators: samples of a system the user left are dropped; none offers no test", "[ui][emulators]") {
    EmulatorsModel model = listed();
    static_cast<void>(model.apply(UiCommand::Activate));
    model.setSamples("psx", samples());
    CHECK_FALSE(model.samplesKnown());
    model.setSamples("gb", {});
    CHECK(model.samplesKnown());
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.buttons() == std::vector<EmulatorsButton>{EmulatorsButton::Save, EmulatorsButton::Cancel});
}

TEST_CASE("ui/emulators: a table that comes back keeps the system and the choice", "[ui][emulators]") {
    EmulatorsModel model = onChoice(1);
    std::vector<EmulatorSystem> rows = table();
    std::swap(rows[0], rows[1]);
    model.setTable(std::move(rows));
    REQUIRE(model.system() != nullptr);
    CHECK(model.system()->name == "gb");
    CHECK(model.focusedSystem() == 1);
    CHECK(model.option()->value == "Home");

    // The system went away while its row showed.
    EmulatorsModel gone = listed();
    static_cast<void>(gone.apply(UiCommand::Activate));
    gone.setTable({});
    CHECK(gone.view() == EmulatorsView::Systems);
}

TEST_CASE("ui/emulators: a list holds no options: they come with the system, and stay over a list that comes back",
          "[ui][emulators]") {
    EmulatorsModel model;
    model.open();
    static_cast<void>(model.takeRequest());
    std::vector<EmulatorSystem> rows = table();
    const std::vector<EmulatorOption> options = rows[0].options;
    rows[0].options.clear();
    rows[1].options.clear();
    const std::vector<EmulatorSystem> bare = rows;
    model.setTable(rows);
    CHECK_FALSE(model.takeRequest().has_value()); // the list asks for nothing
    static_cast<void>(model.apply(UiCommand::Activate));
    CHECK(model.optionRows() == 0);
    model.setOptions("gb", options);
    model.setOptions("nothing", options);
    CHECK(model.optionRows() == 3);
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.option()->value == "Home");

    model.setTable(bare); // after a change
    CHECK(model.optionRows() == 3);
    const auto asked = model.takeRequest();
    REQUIRE(asked.has_value());
    CHECK(asked->kind == EmulatorsCommandKind::Samples);
    CHECK(asked->system == "gb");
    std::vector<EmulatorOption> changed = options;
    changed[1].verified = true;
    model.setOptions("gb", changed);
    CHECK(model.option()->verified);
}

TEST_CASE("ui/emulators: clicks press what the keys press", "[ui][emulators]") {
    EmulatorsModel model = listed();
    EmulatorsClick system;
    system.kind = EmulatorsClick::Kind::System;
    system.index = 1;
    const auto samplesAsked = model.click(system);
    REQUIRE(samplesAsked.has_value());
    CHECK(samplesAsked->system == "psx");
    CHECK(model.view() == EmulatorsView::System);
    system.index = 0;
    CHECK_FALSE(model.click(system).has_value()); // not shown any more

    EmulatorsModel choosing = listed();
    static_cast<void>(choosing.apply(UiCommand::Activate));
    choosing.setSamples("gb", samples());
    EmulatorsClick optionClick;
    optionClick.kind = EmulatorsClick::Kind::Option;
    optionClick.index = 1;
    CHECK_FALSE(choosing.click(optionClick).has_value());
    CHECK(choosing.option()->value == "Home");
    EmulatorsClick sample;
    sample.kind = EmulatorsClick::Kind::Sample;
    sample.index = -1;
    CHECK_FALSE(choosing.click(sample).has_value());
    CHECK(choosing.sample() == 1);
    EmulatorsClick button;
    button.kind = EmulatorsClick::Kind::Button;
    button.button = EmulatorsButton::Program;
    CHECK_FALSE(choosing.click(button).has_value()); // not offered: it is ready
    button.button = EmulatorsButton::Save;
    CHECK(choosing.click(button)->kind == EmulatorsCommandKind::Save);
}

TEST_CASE("ui/emulators: for one title it opens on its system, tests that title, stores for it and closes on Back",
          "[ui][emulators]") {
    EmulatorsModel model;
    model.openForGame("gb", "g7", "Seventh", std::nullopt);
    CHECK(model.forGame());
    CHECK(model.takeRequest()->kind == EmulatorsCommandKind::Load);
    model.setTable(table());
    CHECK(model.view() == EmulatorsView::System);
    CHECK(model.system()->name == "gb");
    const auto asked = model.takeRequest();
    REQUIRE(asked);
    CHECK(asked->kind == EmulatorsCommandKind::Samples);
    CHECK(asked->forGame);
    model.setOptions("gb", table().front().options);
    model.setSamples("gb", samples());
    REQUIRE(model.samples().size() == 1); // the title, whatever the system offers
    CHECK(model.samples()[0].gameId == "g7");
    CHECK_FALSE(model.offersClear()); // nothing stored for it

    static_cast<void>(model.apply(UiCommand::Down));
    static_cast<void>(model.apply(UiCommand::Activate));
    CHECK(model.view() == EmulatorsView::Choice);
    const auto test = model.apply(UiCommand::Activate); // Prova
    REQUIRE(test);
    CHECK(test->kind == EmulatorsCommandKind::Test);
    CHECK(test->game == "g7");
    CHECK(test->forGame);
    CHECK(test->value == "Home");
    model.testStarted();
    model.testEnded("verified");
    CHECK(model.view() == EmulatorsView::System);
    CHECK(model.offersClear()); // stored: "Torna alla scelta del sistema"
    model.setOptions("gb", table().front().options);
    CHECK(model.system()->options[1].chosen); // in use: the title's own
    CHECK_FALSE(model.system()->options[0].chosen);

    // The row that takes it away is the last.
    static_cast<void>(model.apply(UiCommand::End));
    const auto clear = model.apply(UiCommand::Activate);
    REQUIRE(clear);
    CHECK(clear->kind == EmulatorsCommandKind::Clear);
    CHECK(clear->forGame);
    model.changeEnded("", true);
    CHECK_FALSE(model.offersClear());

    const auto closed = model.apply(UiCommand::Back);
    REQUIRE(closed);
    CHECK(closed->kind == EmulatorsCommandKind::Close);
    CHECK_FALSE(model.isOpen());

    // A plain opening forgets the title.
    model.open();
    CHECK_FALSE(model.forGame());
}

TEST_CASE("ui/emulators: a title's choice shows at once; a system gone says so and Back closes", "[ui][emulators]") {
    EmulatorsModel model;
    model.openForGame("gb", "g7", "Seventh", std::pair(false, std::string("Home")));
    model.setTable(table());
    model.setOptions("gb", table().front().options);
    CHECK(model.offersClear());
    CHECK(model.optionRows() == 4);
    CHECK(model.system()->options[1].chosen);

    EmulatorsModel gone;
    gone.openForGame("nes", "g9", "Ninth", std::nullopt);
    gone.setTable(table());
    CHECK(gone.state() == EmulatorsState::Unavailable);
    CHECK(gone.apply(UiCommand::Back)->kind == EmulatorsCommandKind::Close);
}

TEST_CASE("ui/emulators: names", "[ui][emulators]") {
    for (const EmulatorsButton button : {EmulatorsButton::Sample, EmulatorsButton::Test, EmulatorsButton::Program,
                                         EmulatorsButton::ClearProgram, EmulatorsButton::Save, EmulatorsButton::Cancel}) {
        CHECK(emulatorsButtonFromName(toString(button)) == button);
        CHECK(elementIdOf(button).starts_with("em-"));
    }
    CHECK_FALSE(emulatorsButtonFromName("nothing").has_value());
    CHECK(toString(EmulatorsView::Testing) == "testing");
    CHECK(toString(EmulatorsState::Unavailable) == "unavailable");
    CHECK(toString(EmulatorsCommandKind::ClearProgram) == "clear-program");
}
