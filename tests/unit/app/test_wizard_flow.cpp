// The wizard's flow (ADR-0018 §4): when a run starts it, what its jobs bring to the pages, a resumed wizard, the
// folder browser over the disk, library.json never replaced without its copy; --wizard's options.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <rv/app/command_line.hpp>
#include <rv/app/wizard_flow.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::app::CommandLineOptions;
using rv::app::WizardFlow;
using rv::app::WizardStart;
using rv::library::LibraryStage;
using rv::ui::WizardCommandKind;
using rv::ui::WizardFolder;
using rv::ui::WizardWork;

namespace {

rv::core::Paths pathsOf(const fs::path& data) {
    rv::core::PathOptions options;
    options.userDataOverride = data;
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return rv::core::Paths::resolve(options, environment).value();
}

/// Polls until nothing of the flow is awaited.
void settle(WizardFlow& flow) {
    const auto start = std::chrono::steady_clock::now();
    do {
        flow.poll();
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(30)) {
            FAIL("the wizard's job did not answer");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (flow.busy());
    flow.poll();
}

WizardFlow::Options optionsFor(const fs::path& data, const rv::test::Fixture& fixture) {
    WizardFlow::Options options;
    options.paths = pathsOf(data);
    options.esdeHome = fixture.root();
    options.esdeInstall = fixture.path("Programs/ES-DE");
    return options;
}

void press(WizardFlow& flow) {
    if (const auto command = flow.model().apply(rv::ui::UiCommand::Activate)) {
        flow.command(*command);
    }
}

/// Library.json's path, config folder made.
fs::path libraryFile(const fs::path& data) {
    return pathsOf(data).configDir() / rv::esde::kLibraryFileName;
}

} // namespace

TEST_CASE("wizard: a run starts it only where ADR-0018 says", "[wizard]") {
    CommandLineOptions interactive;
    CommandLineOptions limited;
    limited.frameLimit = 10;
    CommandLineOptions asked = limited;
    asked.wizard = true;

    CHECK(rv::app::wizardStart(true, interactive, LibraryStage::NoLibrary) == WizardStart::Fresh);
    CHECK(rv::app::wizardStart(true, interactive, LibraryStage::NoCatalog) == WizardStart::Resume);
    CHECK(rv::app::wizardStart(true, interactive, LibraryStage::Ready) == WizardStart::None);
    // A data directory the CLI prepared never sees it; a scene, a model, a mock catalog neither.
    CHECK(rv::app::wizardStart(false, interactive, LibraryStage::NoLibrary) == WizardStart::None);
    // A frame-limited run keeps today's test room...
    CHECK(rv::app::wizardStart(true, limited, LibraryStage::NoLibrary) == WizardStart::None);
    CHECK(rv::app::wizardStart(true, limited, LibraryStage::NoCatalog) == WizardStart::None);
    // ... unless it asks.
    CHECK(rv::app::wizardStart(true, asked, LibraryStage::NoLibrary) == WizardStart::Fresh);
    CHECK(rv::app::wizardStart(true, asked, LibraryStage::NoCatalog) == WizardStart::Resume);
    CHECK(rv::app::wizardStart(true, asked, LibraryStage::Ready) == WizardStart::None);
}

TEST_CASE("wizard: --wizard needs --esde-home, --esde-home needs --wizard", "[wizard]") {
    const auto parse = [](std::vector<std::string> arguments) { return rv::app::parseCommandLine(arguments); };
    CHECK_FALSE(parse({"--wizard"}).has_value());
    CHECK_FALSE(parse({"--wizard", "--smoke"}).has_value());
    CHECK_FALSE(parse({"--esde-home", "C:\\x"}).has_value());
    CHECK_FALSE(parse({"--wizard", "--esde-install", "C:\\x"}).has_value());
    CHECK_FALSE(parse({"--wizard", "--esde-home", "C:\\x", "--scene", "m0_testroom"}).has_value());
    const auto options = parse({"--wizard", "--esde-home", "C:\\x", "--esde-install", "C:\\y"});
    REQUIRE(options.has_value());
    CHECK(options->wizard);
    CHECK(options->esdeHome == fs::path("C:\\x"));
    CHECK(options->esdeInstall == fs::path("C:\\y"));
}

TEST_CASE("wizard: detection and preview reach the pages", "[wizard]") {
    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const rv::test::TempDir data("rv-wizard");
    WizardFlow flow(optionsFor(data.path(), fixture));
    flow.open(WizardStart::Fresh);
    press(flow);
    settle(flow);
    auto& model = flow.model();
    CHECK(model.detectionWork() == WizardWork::Done);
    CHECK(model.detection().problem == "none");
    CHECK(model.detection().folders[static_cast<std::size_t>(WizardFolder::Rom)].state == "ok");
    CHECK(model.canGoOn());

    press(flow);
    settle(flow);
    REQUIRE(model.previewWork() == WizardWork::Done);
    CHECK(model.preview().games > 0);
    CHECK(model.preview().systemsWithGames == 1);
    CHECK(flow.figures().entries >= model.preview().games);
    // Nothing was written yet.
    CHECK_FALSE(fs::exists(libraryFile(data.path())));
}

TEST_CASE("wizard: a ROM folder out of reach stops page 2, a chosen one goes on", "[wizard]") {
    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const rv::test::TempDir data("rv-wizard");
    fs::rename(fixture.path("roms"), fixture.path("roms-moved"));
    WizardFlow flow(optionsFor(data.path(), fixture));
    flow.open(WizardStart::Fresh);
    press(flow);
    settle(flow);
    auto& model = flow.model();
    CHECK(model.detection().problem == "rom-root-unavailable");
    CHECK(model.detection().folders[static_cast<std::size_t>(WizardFolder::Rom)].state == "missing");
    CHECK_FALSE(model.canGoOn());

    // The browser over a vanished folder: it lists up, and the moved folder is chosen.
    rv::ui::WizardCommand browse{WizardCommandKind::Browse};
    browse.folder = WizardFolder::Rom;
    flow.command(browse);
    auto& browser = flow.browser();
    REQUIRE(browser.isOpen());
    flow.poll();
    settle(flow);
    CHECK(browser.state() == rv::ui::FolderBrowserState::Failed);
    CHECK(browser.failure() == rv::ui::FolderBrowserFailure::Missing);
    static_cast<void>(browser.apply(rv::ui::UiCommand::Activate)); // up
    flow.poll();
    settle(flow);
    REQUIRE(browser.state() == rv::ui::FolderBrowserState::Listed);
    CHECK(browser.folder() == fixture.root());
    const auto rows = browser.rows();
    const auto moved = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.name == "roms-moved"; });
    REQUIRE(moved != rows.end());
    rv::ui::FolderBrowserClick click{rv::ui::FolderBrowserClick::Kind::Row};
    click.index = static_cast<std::size_t>(moved - rows.begin());
    static_cast<void>(browser.click(click));
    flow.poll();
    settle(flow);
    const auto chosen = browser.apply(rv::ui::UiCommand::Info);
    REQUIRE(chosen.has_value());
    flow.browserClosed(*chosen);
    settle(flow);
    CHECK(flow.browsed() == 1);
    CHECK(model.detection().problem == "none");
    CHECK(model.detection().folders[static_cast<std::size_t>(WizardFolder::Rom)].path ==
          rv::core::pathToUtf8Lossy(fixture.path("roms-moved")));

    // Page 4 records the override.
    flow.prepareImport();
    settle(flow);
    const auto prepared = flow.takePrepared();
    REQUIRE(prepared.has_value());
    CHECK(prepared->failure.empty());
    CHECK(prepared->changed);
    CHECK(rv::esde::readLibraryFile(libraryFile(data.path()))->romRoot == fixture.path("roms-moved"));
}

TEST_CASE("wizard: a data directory inside the library is refused on page 2", "[wizard]") {
    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    WizardFlow flow(optionsFor(fixture.path("roms/retroverse-data"), fixture));
    flow.open(WizardStart::Fresh);
    press(flow);
    settle(flow);
    CHECK(flow.model().detection().problem == "data-directory-inside");
    CHECK_FALSE(flow.model().canGoOn());
}

TEST_CASE("wizard: resuming with library.json and no database starts at the preview, the file kept", "[wizard]") {
    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const rv::test::TempDir data("rv-wizard");
    const fs::path file = libraryFile(data.path());
    {
        WizardFlow first(optionsFor(data.path(), fixture));
        first.open(WizardStart::Fresh);
        press(first);
        settle(first);
        first.prepareImport();
        settle(first);
        REQUIRE(first.takePrepared().has_value());
    }
    const std::string before = rv::core::readTextFile(file).value();

    WizardFlow::Options options;
    options.paths = pathsOf(data.path()); // no --esde-home: library.json says
    WizardFlow flow(options);
    flow.open(WizardStart::Resume);
    auto& model = flow.model();
    CHECK(model.page() == rv::ui::WizardPage::Preview);
    const auto entered = model.takeEntered();
    REQUIRE(entered.has_value());
    flow.command(*entered);
    settle(flow);
    REQUIRE(model.previewWork() == WizardWork::Done);
    CHECK(model.preview().games > 0);

    flow.prepareImport();
    settle(flow);
    const auto prepared = flow.takePrepared();
    REQUIRE(prepared.has_value());
    CHECK(prepared->failure.empty());
    CHECK_FALSE(prepared->changed); // it stands
    CHECK(rv::core::readTextFile(file).value() == before);
    CHECK_FALSE(fs::exists(file.parent_path() / "backups"));
}

TEST_CASE("wizard: library.json replaced only with its copy; a copy that fails leaves it, and what was chosen, as they are",
          "[wizard]") {
    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const rv::test::TempDir data("rv-wizard");
    const fs::path file = libraryFile(data.path());
    {
        WizardFlow first(optionsFor(data.path(), fixture));
        first.open(WizardStart::Fresh);
        press(first);
        settle(first);
        first.prepareImport();
        settle(first);
        REQUIRE(first.takePrepared().has_value());
    }
    const std::string before = rv::core::readTextFile(file).value();
    WizardFlow flow(optionsFor(data.path(), fixture));
    flow.open(WizardStart::Resume);
    static_cast<void>(flow.model().takeEntered());
    rv::ui::WizardCommand detect{WizardCommandKind::Detect};
    flow.command(detect);
    settle(flow);
    rv::ui::WizardCommand browse{WizardCommandKind::Browse};
    browse.folder = WizardFolder::Media;
    flow.command(browse);
    flow.poll();
    settle(flow);
    REQUIRE(flow.browser().canUse());
    const auto chosen = flow.browser().apply(rv::ui::UiCommand::Info);
    REQUIRE(chosen.has_value());
    flow.browserClosed(*chosen);
    settle(flow);

    // A file where the backups' folder must go: the copy cannot be made.
    const fs::path backups = file.parent_path() / "backups";
    REQUIRE(rv::core::writeTextFileAtomic(backups, "in the way").has_value());
    flow.prepareImport();
    settle(flow);
    auto prepared = flow.takePrepared();
    REQUIRE(prepared.has_value());
    CHECK(prepared->failure == "library-backup");
    CHECK(rv::core::readTextFile(file).value() == before);

    // Out of the way, Riprova writes what was chosen: the failure kept it.
    fs::remove(backups);
    flow.prepareImport();
    settle(flow);
    prepared = flow.takePrepared();
    REQUIRE(prepared.has_value());
    CHECK(prepared->failure.empty());
    CHECK(prepared->changed);
    CHECK(rv::core::readTextFile(file).value() != before);
    CHECK(fs::is_directory(backups));
    CHECK_FALSE(fs::is_empty(backups));
}
