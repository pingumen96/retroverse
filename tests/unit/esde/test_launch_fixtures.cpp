// I2 step 10: the launch resolution on the ES-DE fixtures, on this machine (Win32SystemProbe: the fixtures' emulators
// are found through their static paths, never through anything this machine has): the standard fixture's commands as
// its README expects them (ready, an alternative emulator per game and per system, %HIDEWINDOW% and %STARTDIR%, the
// shell refused, %INJECT% per game, the missing emulator and core, a missing file), ES-DE's AlternativeEmulatorPerGame
// off, Retroverse's choices for a system (a label, a profile, stale ones); the custom-systems fixture's custom find rules
// and quirky systems.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/gamelist.hpp>
#include <rv/esde/launch_translation.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/platform_win/system_probe.hpp>

#include "launch_test_machine.hpp"
#include "support/fixture.hpp"
#include "support/program_stub.hpp"

namespace fs = std::filesystem;
using rv::catalog::ChoiceSource;
using rv::catalog::LaunchReadiness;

namespace {

/// A path's UTF-8 text with '\' separators, as the translation gives paths.
std::string text(const fs::path& path) {
    return rv::core::pathToUtf8(fs::path(path).make_preferred());
}

/// A path from UTF-8 text (std::filesystem reads a narrow literal in the ANSI code page).
fs::path u8path(std::string_view text) {
    return rv::core::pathFromUtf8(text);
}

/// A fixture's library read as `profiles` and `launch` read it.
struct FixtureLibrary {
    rv::test::Fixture fixture;
    rv::catalog::LibraryProfile profile;
    rv::esde::EsdeSettings settings;
    rv::esde::ResolvedRoot romRoot;
    rv::esde::SystemsConfiguration systems;
    rv::esde::FindRules findRules;
    std::vector<rv::catalog::SystemInfo> infos;
    rv::platform_win::Win32SystemProbe probe;
    rv::launcher::EmulatorProfiles profiles;
    std::unique_ptr<rv::esde::EsdeCommandTranslator> translator;

    explicit FixtureLibrary(rv::test::Fixture materialized) : fixture(std::move(materialized)) {}

    /// Reads (again) the configuration: call after changing the materialized copy.
    void load() {
        profile.esdeHome = fixture.path("ES-DE");
        profile.installFolder = fixture.path("Programs/ES-DE");
        auto read = rv::esde::readSettings(profile.esdeHome);
        REQUIRE(read.has_value());
        settings = *read;
        romRoot = rv::esde::resolveRomRoot(settings, profile);
        auto loaded = rv::esde::loadSystems(profile);
        REQUIRE(loaded.has_value());
        systems = *loaded;
        findRules = rv::esde::loadFindRules(profile);
        std::vector<rv::catalog::Issue> issues;
        infos = rv::esde::systemInfos(systems, romRoot, profile, issues);
        translator = std::make_unique<rv::esde::EsdeCommandTranslator>(
            rv::esde::TranslationContext{profile, romRoot, &findRules, &probe}, systems);
    }

    [[nodiscard]] const rv::catalog::SystemInfo& system(std::string_view name) const {
        const auto found = std::find_if(infos.begin(), infos.end(),
                                        [&](const rv::catalog::SystemInfo& info) { return info.name.str() == name; });
        REQUIRE(found != infos.end());
        return *found;
    }

    [[nodiscard]] rv::catalog::LaunchContext context() const {
        return rv::catalog::LaunchContext{translator.get(), &profiles, &probe, nullptr};
    }

    /// The choices of a system and, for a game, its gamelist entry's <altemulator>.
    [[nodiscard]] rv::catalog::LaunchChoices choices(std::string_view name, const std::string& relativePath = {}) const {
        rv::catalog::LaunchChoices result;
        result.alternativeEmulatorPerGame = settings.alternativeEmulatorPerGame;
        const rv::catalog::SystemInfo& info = system(name);
        const auto gamelist = rv::esde::readGamelist(
            rv::esde::gamelistFile(info.name, info.romFolder, settings, profile.esdeHome), info.name, info.romFolder);
        if (gamelist) {
            result.systemAlternativeEmulator = gamelist->alternativeEmulator;
            const auto key = rv::catalog::makeLookupKey(relativePath);
            for (const rv::esde::GamelistEntry& entry : gamelist->entries) {
                if (key && !entry.outsideSystemFolder() && entry.key.key() == key->key()) {
                    result.gameAltEmulator = entry.altEmulator;
                }
            }
        }
        if (const rv::launcher::LaunchChoice* choice = profiles.systemChoice(name)) {
            result.system = *choice;
        }
        return result;
    }

    /// The resolution of a system (no game).
    [[nodiscard]] rv::catalog::LaunchResolution resolveSystem(std::string_view name) const {
        return rv::catalog::resolveLaunch(system(name), nullptr, choices(name), context());
    }

    /// The resolution of a game, by its path relative to the system's ROM folder.
    [[nodiscard]] rv::catalog::LaunchResolution resolveGame(std::string_view name, const std::string& relativePath,
                                                            rv::catalog::LaunchChoices* override = nullptr) const {
        const rv::catalog::SystemInfo& info = system(name);
        rv::catalog::LaunchGame game;
        game.relativePath = relativePath;
        if (const auto listed = rv::esde::listedPath(info.romFolder, relativePath)) {
            game.relativePath = listed->relativePath;
            game.directory = listed->directory;
        } else {
            game.availability = rv::catalog::Availability::Missing;
        }
        const rv::catalog::LaunchChoices chosen = override != nullptr ? *override : choices(name, game.relativePath);
        return rv::catalog::resolveLaunch(info, &game, chosen, context());
    }

    /// A command of a system translated without a game.
    [[nodiscard]] rv::catalog::LaunchTranslation command(std::string_view name, std::string_view label) const {
        return translator->translate(system(name).name, label, nullptr);
    }
};

/// Held by pointer: the translator points into it.
std::unique_ptr<FixtureLibrary> library(std::string_view name) {
    auto fixture = rv::test::Fixture::materialize(name);
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    auto result = std::make_unique<FixtureLibrary>(std::move(*fixture));
    result->load();
    return result;
}

std::vector<std::string> argvOf(const rv::catalog::LaunchResolution& resolution) {
    REQUIRE(resolution.translation.request);
    std::vector<std::string> argv{text(resolution.translation.request->executable)};
    const auto& arguments = resolution.translation.request->arguments;
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    return argv;
}

} // namespace

TEST_CASE("esde/launch fixtures: the standard fixture's systems", "[esde][launch]") {
    const auto standard = library("esde/standard");
    const fs::path emulator = standard->fixture.path("Programs/ES-DE/Emulators/FakeEmulator/fake-emulator.exe");

    const auto gb = standard->resolveSystem("gb");
    INFO(rv::test::blockerText(gb.translation));
    CHECK(gb.readiness() == LaunchReadiness::Ready);
    CHECK(gb.chosen.source == ChoiceSource::FirstCommand);
    CHECK(gb.chosen.choice.value == "Fake Emulator");
    REQUIRE(gb.translation.program);
    CHECK(gb.translation.program->path == emulator);
    CHECK(gb.translation.program->rule == "staticpath");
    CHECK(gb.translation.program->entry == "%ESPATH%\\Emulators\\FakeEmulator\\fake-emulator.exe");
    // What was tried before: App Paths, the executable search, the static path under '~'.
    CHECK(gb.translation.attempts.size() == 3);
    REQUIRE(gb.translation.core);
    CHECK(gb.translation.core->path == emulator.parent_path() / "cores" / "fake_gb_libretro.dll");

    const auto snes = standard->resolveSystem("snes");
    CHECK(snes.readiness() == LaunchReadiness::Ready);
    CHECK(snes.chosen.source == ChoiceSource::EsdeSystem);
    CHECK(snes.chosen.choice.value == "Fake Emulator (Hidden Console)");

    const auto psx = standard->resolveSystem("psx");
    CHECK(psx.readiness() == LaunchReadiness::Ready);
    CHECK(psx.chosen.source == ChoiceSource::FirstCommand);

    // Every other command, without a game.
    CHECK(standard->command("gb", "Fake Emulator (Game Folder)").readiness() == LaunchReadiness::Ready);
    const auto missing = standard->command("gb", "Missing Emulator (Standalone)");
    CHECK(missing.readiness() == LaunchReadiness::EmulatorNotFound);
    CHECK(missing.attempts.size() == 2);
    const auto home = standard->command("gb", "Home Emulator");
    CHECK(home.readiness() == LaunchReadiness::Ready);
    REQUIRE(home.program);
    CHECK(home.program->path == standard->fixture.path("Emulators/HomeEmulator/home-emulator.exe"));
    // I11 step 13: "open the game's file" is each game's (its file opened by Retroverse, never through cmd.exe).
    const auto shell = standard->command("snes", "Shortcut or script");
    CHECK(shell.readiness() == LaunchReadiness::Ready);
    CHECK(shell.blockers.empty());
    CHECK_FALSE(shell.program);
    REQUIRE(shell.notes.size() == 1);
    CHECK(shell.notes.front().find("never through cmd.exe") != std::string::npos);
    CHECK(standard->command("psx", "Fake Emulator (Inject)").readiness() == LaunchReadiness::Ready);
    const auto noCore = standard->command("psx", "Fake Emulator (Missing Core)");
    CHECK(noCore.readiness() == LaunchReadiness::CoreNotFound);
    CHECK(rv::test::blockerText(noCore).find("fake_missing_libretro.dll") != std::string::npos);
}

TEST_CASE("esde/launch fixtures: the standard fixture's games", "[esde][launch]") {
    const auto standard = library("esde/standard");
    const fs::path emulator = standard->fixture.path("Programs/ES-DE/Emulators/FakeEmulator/fake-emulator.exe");
    const fs::path roms = standard->fixture.path("roms");

    // The default command, the path spelled as the folder lists it.
    const auto tennis = standard->resolveGame("gb", "tiny tennis (EUROPE).gb");
    CHECK(tennis.readiness() == LaunchReadiness::Ready);
    CHECK(argvOf(tennis) == std::vector<std::string>{text(emulator), "--", "-L",
                                                     text(emulator.parent_path() / "cores" / "fake_gb_libretro.dll"),
                                                     text(roms / "gb" / "Tiny Tennis (Europe).gb")});
    CHECK(tennis.request()->workingDirectory == emulator.parent_path());
    CHECK(tennis.request()->label == "Tiny Tennis (Europe).gb with Fake Emulator");

    // Café Racer's <altemulator>: %STARTDIR%=%GAMEDIR%.
    const auto cafe = standard->resolveGame("gb", "Café Racer (Europe).gb");
    CHECK(cafe.readiness() == LaunchReadiness::Ready);
    CHECK(cafe.chosen.source == ChoiceSource::EsdeGame);
    CHECK(cafe.chosen.choice.value == "Fake Emulator (Game Folder)");
    CHECK(argvOf(cafe) == std::vector<std::string>{text(emulator), "--", text(roms / "gb" / u8path("Café Racer (Europe).gb"))});
    CHECK(cafe.request()->workingDirectory == roms / "gb");

    // snes: the gamelist's <alternativeEmulator>, a hidden console, the emulator's folder.
    const auto alpha = standard->resolveGame("snes", "Alpha Quest (USA).sfc");
    CHECK(alpha.readiness() == LaunchReadiness::Ready);
    REQUIRE(alpha.request());
    CHECK(alpha.request()->hideConsoleWindow);
    CHECK(alpha.request()->workingDirectory == emulator.parent_path());

    // A gamelist entry whose file is gone.
    const auto gone = standard->resolveGame("gb", "Gone Fishing (USA).gb");
    CHECK(gone.readiness() == LaunchReadiness::RomMissing);
    CHECK_FALSE(gone.request());

    // psx's inject command: Solo Disc has a usable inject file, Twin Tracks none.
    rv::catalog::LaunchChoices inject = standard->choices("psx");
    inject.system = rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::EsdeCommand, "Fake Emulator (Inject)"};
    const auto solo = standard->resolveGame("psx", "Solo Disc (USA).chd", &inject);
    CHECK(solo.readiness() == LaunchReadiness::RequiresConfiguration);
    CHECK(rv::test::blockerCodes(solo.translation) == std::vector<std::string>{"inject-file"});
    const auto twin = standard->resolveGame("psx", "Twin Tracks (Europe).cue", &inject);
    CHECK(twin.readiness() == LaunchReadiness::Ready);
    CHECK(argvOf(twin) == std::vector<std::string>{text(emulator), "--", text(roms / "psx" / "Twin Tracks (Europe).cue")});
}

TEST_CASE("esde/launch fixtures: AlternativeEmulatorPerGame off, Retroverse's choices for a system", "[esde][launch]") {
    auto standard = library("esde/standard");
    const fs::path emulator = standard->fixture.path("Programs/ES-DE/Emulators/FakeEmulator/fake-emulator.exe");

    SECTION("ES-DE's setting off: the game's <altemulator> is not used") {
        REQUIRE(standard->fixture
                    .replaceText("ES-DE/settings/es_settings.xml", R"(name="AlternativeEmulatorPerGame" value="true")",
                                 R"(name="AlternativeEmulatorPerGame" value="false")")
                    .has_value());
        standard->load();
        const auto cafe = standard->resolveGame("gb", "Café Racer (Europe).gb");
        CHECK(cafe.chosen.source == ChoiceSource::FirstCommand);
        REQUIRE(cafe.chosen.warnings.size() == 1);
        CHECK(cafe.chosen.warnings[0].find("AlternativeEmulatorPerGame") != std::string::npos);
    }
    SECTION("emulators.json's systemChoices: a label, a profile, and stale ones") {
        const nlohmann::json user = {
            {"schemaVersion", 1},
            {"profiles",
             {{{"id", "fake"},
               {"displayName", "Fake"},
               {"executable", text(emulator)},
               {"arguments", {"--profile", "{rom}"}},
               {"systems", {{"gb", nlohmann::json::object()}}}}}},
            {"systemChoices",
             {{"gb", {{"profile", "fake"}}},
              {"snes", {{"command", "Fake Emulator"}}},
              {"psx", {{"command", "Gone Emulator"}}}}}};
        standard->profiles = rv::launcher::mergeProfileDocuments(
            {}, rv::launcher::parseProfileDocument(user, "emulators.json", rv::launcher::ProfileOrigin::User));
        REQUIRE(standard->profiles.issues.empty());

        // The game's <altemulator> comes before Retroverse's choice for the system.
        CHECK(standard->resolveGame("gb", "Café Racer (Europe).gb").chosen.source == ChoiceSource::EsdeGame);
        const auto tennis = standard->resolveGame("gb", "Tiny Tennis (Europe).gb");
        CHECK(tennis.chosen.source == ChoiceSource::RetroverseSystem);
        CHECK(tennis.readiness() == LaunchReadiness::Ready);
        CHECK(argvOf(tennis) == std::vector<std::string>{
                                    text(emulator), "--profile",
                                    text(standard->fixture.path("roms/gb/Tiny Tennis (Europe).gb"))});
        REQUIRE(tennis.translation.program);
        CHECK(tennis.translation.program->rule == "path");
        // Retroverse's choice comes before the gamelist's <alternativeEmulator>.
        const auto snes = standard->resolveSystem("snes");
        CHECK(snes.chosen.source == ChoiceSource::RetroverseSystem);
        CHECK(snes.chosen.choice.value == "Fake Emulator");
        // A stored label that no longer exists: requires launcher configuration, naming it.
        const auto psx = standard->resolveSystem("psx");
        CHECK(psx.readiness() == LaunchReadiness::RequiresConfiguration);
        CHECK(rv::test::blockerCodes(psx.translation) == std::vector<std::string>{"choice-stale"});
        CHECK(rv::test::blockerText(psx.translation).find("Gone Emulator") != std::string::npos);
        // A profile that no longer exists.
        standard->profiles.profiles.clear();
        const auto gb = standard->resolveSystem("gb");
        CHECK(rv::test::blockerCodes(gb.translation) == std::vector<std::string>{"profile-unknown"});
    }
}

TEST_CASE("esde/launch fixtures: the custom-systems fixture's custom find rules and quirky systems", "[esde][launch]") {
    auto custom = library("esde/custom-systems");
    const fs::path roms = custom->fixture.path("roms");

    // gb is the custom definition; FAKE-EMULATOR the custom rule (~\Custom Emulators), not the bundled %ESPATH% one.
    CHECK(custom->command("gb", "Custom Emulator").readiness() == LaunchReadiness::EmulatorNotFound);
    CHECK(custom->command("gb", "Bundled Emulator").readiness() == LaunchReadiness::EmulatorNotFound);
    const fs::path fake = rv::test::writeProgramStub(custom->fixture.root() / "Custom Emulators" / "fake-emulator.exe");
    const auto bundled = custom->resolveGame("gb", "Custom Climb (World).gbx");
    CHECK(bundled.readiness() == LaunchReadiness::EmulatorNotFound); // gb's first command is "Custom Emulator"
    rv::catalog::LaunchChoices second = custom->choices("gb");
    second.system = rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::EsdeCommand, "Bundled Emulator"};
    const auto climb = custom->resolveGame("gb", "Custom Climb (World).gbx", &second);
    CHECK(climb.readiness() == LaunchReadiness::Ready);
    CHECK(argvOf(climb) ==
          std::vector<std::string>{text(fake), "--", text(roms / "gb-custom" / "Custom Climb (World).gbx")});

    // CUSTOM-EMULATOR: the wildcard under %ESPATH%; a '|' replacement program requires launcher configuration.
    const fs::path pipeTarget = rv::test::writeProgramStub(custom->fixture.path("Emulators/custom-emulator.exe"));
    auto translation = custom->command("homebrew", "Custom Emulator");
    CHECK(translation.readiness() == LaunchReadiness::RequiresConfiguration);
    CHECK(rv::test::blockerCodes(translation) == std::vector<std::string>{"find-rule-undecided"});
    const fs::path wildcard =
        rv::test::writeProgramStub(custom->fixture.path("Programs/ES-DE/Emulators/Custom/custom-emulator-1.2.exe"));
    translation = custom->command("homebrew", "Custom Emulator");
    CHECK(translation.readiness() == LaunchReadiness::Ready);
    REQUIRE(translation.program);
    CHECK(translation.program->path == wildcard);
    (void)pipeTarget;

    // nes: the custom RETROARCH core folder (~\Custom Cores).
    translation = custom->command("nes", "Fake Emulator (Core)");
    CHECK(translation.readiness() == LaunchReadiness::CoreNotFound);
    rv::test::writeProgramStub(custom->fixture.root() / "Custom Cores" / "fake_nes_libretro.dll");
    translation = custom->command("nes", "Fake Emulator (Core)");
    CHECK(translation.readiness() == LaunchReadiness::Ready);
    REQUIRE(translation.core);
    CHECK(translation.core->path == custom->fixture.root() / "Custom Cores" / "fake_nes_libretro.dll");
    CHECK(custom->command("nes", "Shell").readiness() == LaunchReadiness::Ready); // each game's file (I11 step 13)

    // A placeholder, a game without an extension, the labels ES-DE keeps.
    const auto android = custom->resolveSystem("androidgames");
    // Its ROM folder does not exist either.
    CHECK(rv::test::blockerCodes(android.translation) ==
          std::vector<std::string>{"source-unavailable", "command-placeholder"});
    const auto runme = custom->resolveGame("dirgames", "runme");
    CHECK(runme.readiness() == LaunchReadiness::Ready); // found through the custom rule, ~\Custom Emulators
    CHECK(argvOf(runme) == std::vector<std::string>{text(fake), text(roms / "dirgames" / "runme")});
    const auto other = custom->command("duplabels", "Other");
    CHECK(other.readiness() == LaunchReadiness::Ready);
    CHECK(custom->translator->commandLabels(custom->system("duplabels").name) ==
          std::vector<std::string>{"Same", "Other"});
    CHECK(custom->translator->commandLabels(custom->system("dirgames").name) == std::vector<std::string>{""});
}
