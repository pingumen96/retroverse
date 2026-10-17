// I2 step 10: the catalog's launch resolution (ADR-0008 §8) on a fake translator: the choice by specificity (Retroverse's
// choice for the game, the game's <altemulator> with AlternativeEmulatorPerGame, Retroverse's choice for the system, the
// gamelist's <alternativeEmulator>, the first command), stale labels (Retroverse's block, ES-DE's fall back with a
// warning), the readiness precedence, the game's and the system's availability, and profiles planned with rv_launcher.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/core/text.hpp>
#include <rv/launcher/emulator_profile.hpp>

#include "support/fake_system_probe.hpp"
#include "support/program_stub.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::catalog::ChoiceSource;
using rv::catalog::LaunchChoices;
using rv::catalog::LaunchReadiness;
using rv::catalog::ReadinessBlocker;
using rv::launcher::LaunchChoice;
using rv::launcher::LaunchChoiceKind;

namespace {

/// A translator that answers what each label was set to give, and records what it was asked.
class FakeTranslator final : public rv::catalog::ICommandTranslator {
public:
    std::vector<std::string> labels{"First", "Second", "Third"};
    std::vector<ReadinessBlocker> blockersOf; ///< given to every translation
    mutable std::vector<std::string> asked;

    std::optional<std::vector<std::string>> commandLabels(const rv::catalog::SystemName& system) const override {
        if (system.str() != "gb") {
            return std::nullopt;
        }
        return labels;
    }
    rv::catalog::LaunchTranslation translate(const rv::catalog::SystemName&, std::string_view label,
                                             const rv::catalog::LaunchGame* game) const override {
        asked.emplace_back(label);
        rv::catalog::LaunchTranslation translation;
        translation.blockers = blockersOf;
        if (game != nullptr) {
            rv::launcher::LaunchRequest request;
            request.executable = "C:\\Emu\\emu.exe";
            request.arguments = {std::string(label), game->relativePath};
            translation.request = request;
        }
        return translation;
    }
};

LaunchChoice command(std::string label) {
    return LaunchChoice{LaunchChoiceKind::EsdeCommand, std::move(label)};
}
LaunchChoice profile(std::string id) {
    return LaunchChoice{LaunchChoiceKind::Profile, std::move(id)};
}

rv::launcher::EmulatorProfiles profilesWith(std::vector<std::string> ids) {
    rv::launcher::EmulatorProfiles profiles;
    for (std::string& id : ids) {
        rv::launcher::EmulatorProfile entry;
        entry.id = std::move(id);
        entry.displayName = entry.id;
        profiles.profiles.push_back(std::move(entry));
    }
    return profiles;
}

rv::catalog::SystemInfo gbSystem(const fs::path& romFolder = "C:\\ROMs\\gb") {
    rv::catalog::SystemInfo system;
    system.name = *rv::catalog::SystemName::parse("gb");
    system.romFolder = romFolder;
    return system;
}

} // namespace

TEST_CASE("catalog/launch: readiness names and precedence", "[catalog][launch]") {
    using enum LaunchReadiness;
    for (const LaunchReadiness readiness :
         {SourceUnavailable, RomMissing, RequiresConfiguration, EmulatorNotFound, CoreNotFound, Ready}) {
        CHECK(rv::catalog::parseLaunchReadiness(rv::catalog::toString(readiness)) == readiness);
    }
    CHECK(rv::catalog::toString(RequiresConfiguration) == "requires-configuration");
    CHECK(rv::catalog::describe(RequiresConfiguration) == "requires launcher configuration");
    CHECK(rv::catalog::readinessOf({}) == Ready);
    const std::vector<ReadinessBlocker> blockers{{CoreNotFound, "core-not-found", ""},
                                                 {EmulatorNotFound, "emulator-not-found", ""},
                                                 {RequiresConfiguration, "token-unsupported", ""}};
    CHECK(rv::catalog::readinessOf(blockers) == RequiresConfiguration);
    const std::vector<ReadinessBlocker> source{{RomMissing, "rom-missing", ""}, {SourceUnavailable, "x", ""}};
    CHECK(rv::catalog::readinessOf(source) == SourceUnavailable);
    for (const ChoiceSource choice : {ChoiceSource::RetroverseGame, ChoiceSource::EsdeGame,
                                      ChoiceSource::RetroverseSystem, ChoiceSource::EsdeSystem,
                                      ChoiceSource::FirstCommand}) {
        CHECK(rv::catalog::parseChoiceSource(rv::catalog::toString(choice)) == choice);
    }
}

TEST_CASE("catalog/launch: the choice of command, most specific first", "[catalog][launch]") {
    const std::vector<std::string> labels{"First", "Second", "Third", "Fourth"};
    const rv::launcher::EmulatorProfiles profiles = profilesWith({"mgba"});
    LaunchChoices all;
    all.game = command("Fourth");
    all.gameAltEmulator = "Third";
    all.system = command("Second");
    all.systemAlternativeEmulator = "First";

    auto chosen = rv::catalog::chooseLaunch(all, labels, profiles);
    CHECK(chosen.source == ChoiceSource::RetroverseGame);
    CHECK(chosen.choice == command("Fourth"));
    CHECK_FALSE(chosen.blocker);
    all.game.reset();
    chosen = rv::catalog::chooseLaunch(all, labels, profiles);
    CHECK(chosen.source == ChoiceSource::EsdeGame);
    CHECK(chosen.choice == command("Third"));
    all.gameAltEmulator.clear();
    chosen = rv::catalog::chooseLaunch(all, labels, profiles);
    CHECK(chosen.source == ChoiceSource::RetroverseSystem);
    CHECK(chosen.choice == command("Second"));
    all.system.reset();
    chosen = rv::catalog::chooseLaunch(all, labels, profiles);
    CHECK(chosen.source == ChoiceSource::EsdeSystem);
    CHECK(chosen.choice == command("First"));
    all.systemAlternativeEmulator.clear();
    chosen = rv::catalog::chooseLaunch(all, labels, profiles);
    CHECK(chosen.source == ChoiceSource::FirstCommand);
    CHECK(chosen.choice == command("First"));
    CHECK(chosen.warnings.empty());

    // A profile id, Retroverse's for the game or the system.
    LaunchChoices profileChoice;
    profileChoice.system = profile("mgba");
    chosen = rv::catalog::chooseLaunch(profileChoice, labels, profiles);
    CHECK(chosen.source == ChoiceSource::RetroverseSystem);
    CHECK(chosen.choice == profile("mgba"));
    CHECK_FALSE(chosen.blocker);
}

TEST_CASE("catalog/launch: stale labels and AlternativeEmulatorPerGame", "[catalog][launch]") {
    const std::vector<std::string> labels{"First", "Second"};
    const rv::launcher::EmulatorProfiles profiles = profilesWith({"mgba"});

    SECTION("Retroverse's stored label or profile that no longer exists blocks, naming it") {
        LaunchChoices choices;
        choices.system = command("Removed");
        choices.systemAlternativeEmulator = "Second";
        auto chosen = rv::catalog::chooseLaunch(choices, labels, profiles);
        CHECK(chosen.source == ChoiceSource::RetroverseSystem);
        REQUIRE(chosen.blocker);
        CHECK(chosen.blocker->readiness == LaunchReadiness::RequiresConfiguration);
        CHECK(chosen.blocker->code == "choice-stale");
        CHECK(chosen.blocker->message.find("\"Removed\"") != std::string::npos);
        choices.game = profile("gone");
        chosen = rv::catalog::chooseLaunch(choices, labels, profiles);
        CHECK(chosen.source == ChoiceSource::RetroverseGame);
        REQUIRE(chosen.blocker);
        CHECK(chosen.blocker->code == "profile-unknown");
        CHECK(chosen.blocker->message.find("'gone'") != std::string::npos);
        // Labels compare exactly, as ES-DE compares them.
        choices.game = command("first");
        CHECK(rv::catalog::chooseLaunch(choices, labels, profiles).blocker->code == "choice-stale");
    }
    SECTION("ES-DE's own stale labels fall back to the first command, with ES-DE's warning, as ES-DE does") {
        // An invalid game's <altemulator>: ES-DE launches the first command, never the gamelist's
        // <alternativeEmulator> (an "else if" of the game's in FileData::launchGame).
        LaunchChoices choices;
        choices.gameAltEmulator = "Gone (Game)";
        choices.systemAlternativeEmulator = "Second";
        auto chosen = rv::catalog::chooseLaunch(choices, labels, profiles);
        CHECK(chosen.source == ChoiceSource::FirstCommand);
        CHECK(chosen.choice == command("First"));
        REQUIRE(chosen.warnings.size() == 1);
        CHECK(chosen.warnings[0].find("Gone (Game)") != std::string::npos);
        CHECK(chosen.warnings[0].find("first command") != std::string::npos);
        CHECK_FALSE(chosen.blocker);
        // An invalid gamelist's label is not looked at either then: one warning.
        choices.systemAlternativeEmulator = "Gone (System)";
        chosen = rv::catalog::chooseLaunch(choices, labels, profiles);
        CHECK(chosen.source == ChoiceSource::FirstCommand);
        REQUIRE(chosen.warnings.size() == 1);
        // Retroverse's choice for the system stands in for ES-DE's default.
        choices.system = command("Second");
        chosen = rv::catalog::chooseLaunch(choices, labels, profiles);
        CHECK(chosen.source == ChoiceSource::RetroverseSystem);
        CHECK(chosen.choice == command("Second"));
        CHECK(chosen.warnings.size() == 1);
        // An invalid gamelist's <alternativeEmulator> alone: ES-DE marks it invalid and launches the first command.
        LaunchChoices systemOnly;
        systemOnly.systemAlternativeEmulator = "Gone (System)";
        chosen = rv::catalog::chooseLaunch(systemOnly, labels, profiles);
        CHECK(chosen.source == ChoiceSource::FirstCommand);
        CHECK(chosen.choice == command("First"));
        REQUIRE(chosen.warnings.size() == 1);
        CHECK(chosen.warnings[0].find("Gone (System)") != std::string::npos);
        CHECK_FALSE(chosen.blocker);
    }
    SECTION("AlternativeEmulatorPerGame off leaves the game's <altemulator> unused") {
        LaunchChoices choices;
        choices.gameAltEmulator = "Second";
        choices.alternativeEmulatorPerGame = false;
        const auto chosen = rv::catalog::chooseLaunch(choices, labels, profiles);
        CHECK(chosen.source == ChoiceSource::FirstCommand);
        REQUIRE(chosen.warnings.size() == 1);
        CHECK(chosen.warnings[0].find("AlternativeEmulatorPerGame") != std::string::npos);
    }
    SECTION("a system without commands") {
        const auto chosen = rv::catalog::chooseLaunch({}, {}, profiles);
        REQUIRE(chosen.blocker);
        CHECK(chosen.blocker->code == "no-command");
    }
}

TEST_CASE("catalog/launch: resolveLaunch: availability, choice and translation", "[catalog][launch]") {
    FakeTranslator translator;
    const rv::launcher::EmulatorProfiles profiles = profilesWith({});
    rv::test::FakeSystemProbe probe;
    const rv::catalog::LaunchContext context{&translator, &profiles, &probe, nullptr};
    rv::catalog::SystemInfo system = gbSystem();
    rv::catalog::LaunchGame game;
    game.relativePath = "Sub/Game.gb";
    LaunchChoices choices;
    choices.systemAlternativeEmulator = "Second";

    auto resolution = rv::catalog::resolveLaunch(system, &game, choices, context);
    CHECK(resolution.readiness() == LaunchReadiness::Ready);
    CHECK(resolution.chosen.source == ChoiceSource::EsdeSystem);
    CHECK(translator.asked == std::vector<std::string>{"Second"});
    REQUIRE(resolution.request());
    CHECK(resolution.request()->arguments == std::vector<std::string>{"Second", "Sub/Game.gb"});

    // A missing file is "ROM missing" whatever the translation says; the request is not given.
    translator.blockersOf = {{LaunchReadiness::CoreNotFound, "core-not-found", "no core"}};
    game.availability = rv::catalog::Availability::Missing;
    resolution = rv::catalog::resolveLaunch(system, &game, choices, context);
    CHECK(resolution.readiness() == LaunchReadiness::RomMissing);
    CHECK(resolution.translation.blockers.front().code == "rom-missing");
    CHECK(resolution.translation.blockers.size() == 2);
    CHECK(resolution.translation.request); // a dry run still shows it
    CHECK_FALSE(resolution.request());

    // A folder that cannot be read comes first.
    game.availability = rv::catalog::Availability::SourceUnavailable;
    game.unavailableReason = rv::catalog::SourceUnavailableReason::Unreachable;
    CHECK(rv::catalog::resolveLaunch(system, &game, choices, context).readiness() ==
          LaunchReadiness::SourceUnavailable);
    system.availability = rv::catalog::Availability::SourceUnavailable;
    system.unavailableReason = rv::catalog::SourceUnavailableReason::FolderMissing;
    resolution = rv::catalog::resolveLaunch(system, nullptr, choices, context);
    CHECK(resolution.readiness() == LaunchReadiness::SourceUnavailable);
    CHECK(resolution.translation.blockers.front().message.find("does not exist") != std::string::npos);

    // A stale Retroverse choice translates nothing.
    translator.asked.clear();
    choices.system = command("Removed");
    resolution = rv::catalog::resolveLaunch(gbSystem(), nullptr, choices, context);
    CHECK(resolution.readiness() == LaunchReadiness::RequiresConfiguration);
    CHECK(translator.asked.empty());

    // A system the library does not configure.
    rv::catalog::SystemInfo unknown = gbSystem();
    unknown.name = *rv::catalog::SystemName::parse("nes");
    CHECK(rv::catalog::resolveLaunch(unknown, nullptr, {}, context).translation.blockers.back().code ==
          "system-unknown");
}

TEST_CASE("catalog/launch: profiles planned with rv_launcher, their blockers mapped to readiness", "[catalog][launch]") {
    rv::test::TempDir dir("rv-launch-profile");
    rv::test::FakeSystemProbe probe;
    const fs::path exe = rv::test::writeProgramStub(dir.path() / "mGBA" / "mGBA.exe");
    probe.addFile(exe);
    const fs::path romFolder = dir.path() / "ROMs" / "gb";
    fs::create_directories(romFolder);
    const nlohmann::json document = {
        {"schemaVersion", 1},
        {"profiles",
         {{{"id", "mgba"},
           {"executable", rv::core::pathToUtf8(exe)},
           {"arguments", {"-f", "{rom}"}},
           {"requiredFiles", {"{var:bios}"}},
           {"systems", {{"gb", {{"variables", {{"bios", rv::core::pathToUtf8(dir.path() / "gb_bios.bin")}}}}}}}},
          {{"id", "lost"}, {"executable", "C:\\Nowhere\\lost.exe"}, {"arguments", {"{rom}"}}},
          {{"id", "typo"}, {"executable", rv::core::pathToUtf8(exe)}, {"arguments", {"{var:missing}"}}}}}};
    const rv::launcher::EmulatorProfiles profiles = rv::launcher::mergeProfileDocuments(
        {}, rv::launcher::parseProfileDocument(document, "emulators.json", rv::launcher::ProfileOrigin::User));
    REQUIRE(profiles.issues.empty());
    FakeTranslator translator;
    const rv::catalog::LaunchContext context{&translator, &profiles, &probe, nullptr};
    const rv::catalog::SystemInfo system = gbSystem(romFolder);
    rv::catalog::LaunchGame game;
    game.relativePath = "Sub/Mole (USA).gb";

    auto translation = rv::catalog::translateProfile(system, &game, *profiles.find("mgba"), context);
    CHECK(translation.readiness() == LaunchReadiness::CoreNotFound);
    CHECK(translation.blockers.front().code == "core-not-found");
    REQUIRE(translation.program);
    CHECK(translation.program->rule == "path");
    probe.addFile(dir.path() / "gb_bios.bin");
    translation = rv::catalog::translateProfile(system, &game, *profiles.find("mgba"), context);
    CHECK(translation.readiness() == LaunchReadiness::Ready);
    REQUIRE(translation.request);
    CHECK(translation.request->arguments ==
          std::vector<std::string>{"-f", rv::core::pathToUtf8(romFolder / "Sub" / "Mole (USA).gb")});
    // A system's readiness: no request.
    translation = rv::catalog::translateProfile(system, nullptr, *profiles.find("mgba"), context);
    CHECK(translation.readiness() == LaunchReadiness::Ready);
    CHECK_FALSE(translation.request);
    CHECK(rv::catalog::translateProfile(system, &game, *profiles.find("lost"), context).readiness() ==
          LaunchReadiness::EmulatorNotFound);
    const auto typo = rv::catalog::translateProfile(system, &game, *profiles.find("typo"), context);
    CHECK(typo.readiness() == LaunchReadiness::RequiresConfiguration);
    CHECK(typo.blockers.front().code == "profile-variable-missing");

    // Through resolveLaunch: Retroverse's choice for the game.
    LaunchChoices choices;
    choices.game = profile("mgba");
    const auto resolution = rv::catalog::resolveLaunch(system, &game, choices, context);
    CHECK(resolution.chosen.source == ChoiceSource::RetroverseGame);
    CHECK(resolution.readiness() == LaunchReadiness::Ready);
    CHECK(translator.asked.empty());
}
