// The wizard's model (ADR-0018 §4): the pages' states, what each page offers, the ways back, a resumed wizard, clicks
// mirroring the keys.

#include <rv/ui/wizard_model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <vector>

using rv::ui::UiCommand;
using rv::ui::WizardButton;
using rv::ui::WizardCommand;
using rv::ui::WizardCommandKind;
using rv::ui::WizardDetection;
using rv::ui::WizardFolder;
using rv::ui::WizardImport;
using rv::ui::WizardModel;
using rv::ui::WizardPage;
using rv::ui::WizardPreview;
using rv::ui::WizardWork;

namespace {

bool offers(const WizardModel& model, WizardButton button) {
    const std::vector<WizardButton> shown = model.buttons();
    return std::find(shown.begin(), shown.end(), button) != shown.end();
}

/// Presses the page's way forward, as a pad does: focused, it is one Activate.
std::optional<WizardCommand> forward(WizardModel& model) {
    REQUIRE(model.focused() == (model.page() == WizardPage::World ? WizardButton::Enter : WizardButton::Next));
    return model.apply(UiCommand::Activate);
}

void toDetection(WizardModel& model) {
    model.open(false);
    const auto entered = forward(model);
    REQUIRE(entered.has_value());
    REQUIRE(entered->kind == WizardCommandKind::Detect);
}

void detected(WizardModel& model, std::string_view problem = "none") {
    WizardDetection detection;
    detection.problem = std::string(problem);
    model.setDetection(WizardWork::Done, detection);
}

} // namespace

TEST_CASE("ui/wizard: the seven pages to the end, each asking for its work", "[ui][wizard]") {
    WizardModel model;
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value()); // closed
    model.open(false);
    CHECK(model.page() == WizardPage::Welcome);
    CHECK_FALSE(model.takeEntered().has_value());

    auto command = forward(model);
    REQUIRE(command.has_value());
    CHECK(command->kind == WizardCommandKind::Detect);
    CHECK(model.page() == WizardPage::Detection);
    CHECK_FALSE(offers(model, WizardButton::Next)); // nothing found yet
    model.setDetection(WizardWork::Running, {});
    CHECK_FALSE(offers(model, WizardButton::Retry));
    detected(model);
    CHECK(model.focused() == WizardButton::Next);

    command = forward(model);
    REQUIRE(command.has_value());
    CHECK(command->kind == WizardCommandKind::Read);
    CHECK(model.page() == WizardPage::Preview);
    model.setPreview(WizardWork::Running, {});
    CHECK_FALSE(offers(model, WizardButton::Next));
    model.setPreview(WizardWork::Done, WizardPreview{});
    CHECK(model.focused() == WizardButton::Next);

    command = forward(model);
    REQUIRE(command.has_value());
    CHECK(command->kind == WizardCommandKind::Import);
    model.setImport(WizardWork::Running, {});
    CHECK(model.buttons().empty()); // nothing to press while it imports
    model.setImport(WizardWork::Done, WizardImport{});
    CHECK(model.focused() == WizardButton::Next);

    command = forward(model);
    REQUIRE(command.has_value());
    CHECK(command->kind == WizardCommandKind::Emulators); // its counts
    CHECK(model.page() == WizardPage::Emulators);
    CHECK(model.focused() == WizardButton::Next);
    model.setEmulators(44, 4);
    CHECK(model.emulatorsReady() == std::optional<std::size_t>(44));
    CHECK(model.emulatorsBlocked() == 4);
    CHECK_FALSE(model.apply(UiCommand::Up).has_value());
    CHECK(model.focused() == WizardButton::Emulators);
    command = model.apply(UiCommand::Activate);
    REQUIRE(command.has_value());
    CHECK(command->kind == WizardCommandKind::OpenEmulators); // the screen over the page
    CHECK(model.page() == WizardPage::Emulators);
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK_FALSE(forward(model).has_value()); // skippable: the menu has it
    CHECK(model.page() == WizardPage::Appearance);
    CHECK(model.focused() == WizardButton::Next);

    command = forward(model);
    REQUIRE(command.has_value());
    CHECK(command->kind == WizardCommandKind::StartWorld);
    CHECK(model.page() == WizardPage::World);
    CHECK(model.buttons().empty());
    model.setWorld(WizardWork::Running);
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    model.setWorld(WizardWork::Done);
    command = forward(model);
    REQUIRE(command.has_value());
    CHECK(command->kind == WizardCommandKind::Enter);
}

TEST_CASE("ui/wizard: a problem keeps Avanti away; Riprova looks again; a folder opens the browser", "[ui][wizard]") {
    WizardModel model;
    toDetection(model);
    detected(model, "rom-root-unavailable");
    CHECK_FALSE(model.canGoOn());
    CHECK_FALSE(offers(model, WizardButton::Next));
    CHECK(offers(model, WizardButton::Retry));
    CHECK_FALSE(model.click(WizardButton::Next).has_value());

    const auto retry = model.click(WizardButton::Retry);
    REQUIRE(retry.has_value());
    CHECK(retry->kind == WizardCommandKind::Detect);

    CHECK(model.buttons().front() == WizardButton::FolderHome);
    static_cast<void>(model.click(WizardButton::FolderHome));
    static_cast<void>(model.apply(UiCommand::Down));
    static_cast<void>(model.apply(UiCommand::Down));
    const auto browse = model.apply(UiCommand::Activate);
    REQUIRE(browse.has_value());
    CHECK(browse->kind == WizardCommandKind::Browse);
    CHECK(browse->folder == WizardFolder::Rom);

    detected(model, "none"); // fixed: A goes on from where the focus stands, not by surprise
    CHECK(offers(model, WizardButton::Next));
}

TEST_CASE("ui/wizard: the ways back", "[ui][wizard]") {
    WizardModel model;
    model.open(false);
    // Back on the first page rests on Esci: nothing ends by mistake.
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.page() == WizardPage::Welcome);
    CHECK(model.focused() == WizardButton::Quit);
    const auto quit = model.apply(UiCommand::Activate);
    REQUIRE(quit.has_value());
    CHECK(quit->kind == WizardCommandKind::Quit);

    toDetection(model);
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.page() == WizardPage::Welcome);

    toDetection(model);
    detected(model);
    static_cast<void>(forward(model));
    const auto again = model.apply(UiCommand::Back); // the preview goes back to a fresh look
    REQUIRE(again.has_value());
    CHECK(again->kind == WizardCommandKind::Detect);
    CHECK(model.page() == WizardPage::Detection);

    // The import cannot be left while it runs, and once done it stands.
    detected(model);
    static_cast<void>(forward(model));
    model.setPreview(WizardWork::Done, {});
    static_cast<void>(forward(model));
    model.setImport(WizardWork::Running, {});
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.page() == WizardPage::Import);
    model.setImport(WizardWork::Failed, {}, "read");
    CHECK(model.failure() == "read");
    CHECK(model.focused() == WizardButton::Retry);
    const auto retried = model.apply(UiCommand::Activate);
    REQUIRE(retried.has_value());
    CHECK(retried->kind == WizardCommandKind::Import);
    const auto read = model.apply(UiCommand::Back);
    REQUIRE(read.has_value());
    CHECK(read->kind == WizardCommandKind::Read);
    CHECK(model.page() == WizardPage::Preview);

    model.setPreview(WizardWork::Done, {});
    static_cast<void>(forward(model));
    model.setImport(WizardWork::Done, {});
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.page() == WizardPage::Import);
    static_cast<void>(forward(model));
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.page() == WizardPage::Emulators);
    static_cast<void>(forward(model));
    CHECK(model.apply(UiCommand::Back)->kind == WizardCommandKind::Emulators); // its counts again
    CHECK(model.page() == WizardPage::Emulators);
    static_cast<void>(forward(model));
    static_cast<void>(forward(model));
    CHECK(model.page() == WizardPage::World);
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.page() == WizardPage::World); // the world has started
}

TEST_CASE("ui/wizard: a resumed wizard opens at the preview, which asks for its read", "[ui][wizard]") {
    WizardModel model;
    model.open(true);
    CHECK(model.resumed());
    CHECK(model.page() == WizardPage::Preview);
    const auto entered = model.takeEntered();
    REQUIRE(entered.has_value());
    CHECK(entered->kind == WizardCommandKind::Read);
    CHECK_FALSE(model.takeEntered().has_value());
    CHECK(offers(model, WizardButton::Previous));
    model.setPreview(WizardWork::Failed, {}, "library-unreadable");
    CHECK(model.focused() == WizardButton::Retry);
}

TEST_CASE("ui/wizard: page 6's rows step by keys and by clicks alike", "[ui][wizard]") {
    WizardModel model;
    toDetection(model);
    detected(model);
    static_cast<void>(forward(model));
    model.setPreview(WizardWork::Done, {});
    static_cast<void>(forward(model));
    model.setImport(WizardWork::Done, {});
    static_cast<void>(forward(model));
    static_cast<void>(forward(model));
    REQUIRE(model.page() == WizardPage::Appearance);

    static_cast<void>(model.click(WizardButton::ReduceMotion));
    static_cast<void>(model.apply(UiCommand::Up));
    REQUIRE(model.focused() == WizardButton::Lighting);
    const auto left = model.apply(UiCommand::Left);
    REQUIRE(left.has_value());
    CHECK(left->kind == WizardCommandKind::Lighting);
    CHECK(left->direction == -1);
    const auto arrow = model.click(WizardButton::Lighting, 1);
    REQUIRE(arrow.has_value());
    CHECK(arrow->direction == 1);
    static_cast<void>(model.apply(UiCommand::Down));
    const auto toggle = model.apply(UiCommand::Activate);
    REQUIRE(toggle.has_value());
    CHECK(toggle->kind == WizardCommandKind::ReduceMotion);
    // A button another page offers does nothing here.
    CHECK_FALSE(model.click(WizardButton::Enter).has_value());
}
