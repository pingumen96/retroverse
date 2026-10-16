// es_systems.xml and es_find_rules.xml (I2 step 2), read as ES-DE 3.4.1 reads them: the standard fixture's systems and
// their SystemInfo values; custom systems replacing bundled ones, <loadExclusive/>, ES-DE's label rules and the
// upstream quirks (custom-systems fixture); ROM folders built as ES-DE builds them (custom-dirs fixture); find rules,
// custom ones first.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::esde::ConfigOrigin;
using rv::esde::FindRuleType;
using rv::esde::SystemDefinition;
using rv::esde::SystemsConfiguration;

namespace {

rv::test::Fixture materialize(std::string_view name) {
    auto fixture = rv::test::Fixture::materialize(name);
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    return std::move(*fixture);
}

rv::catalog::LibraryProfile profileOf(const rv::test::Fixture& fixture) {
    rv::catalog::LibraryProfile profile;
    profile.esdeHome = fixture.path("ES-DE");
    profile.installFolder = fixture.path("Programs/ES-DE");
    return profile;
}

SystemsConfiguration systemsOf(const rv::catalog::LibraryProfile& profile) {
    auto systems = rv::esde::loadSystems(profile);
    INFO((systems ? "read" : systems.error().toString()));
    REQUIRE(systems.has_value());
    return std::move(*systems);
}

rv::esde::ResolvedRoot romRootOf(const rv::catalog::LibraryProfile& profile) {
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    return rv::esde::resolveRomRoot(*settings, profile);
}

std::vector<std::string> namesOf(const std::vector<SystemDefinition>& systems) {
    std::vector<std::string> names;
    for (const SystemDefinition& system : systems) {
        names.push_back(system.name.str());
    }
    return names;
}

std::vector<std::string> labelsOf(const SystemDefinition& system) {
    std::vector<std::string> labels;
    for (const auto& command : system.commands) {
        labels.push_back(command.label);
    }
    return labels;
}

std::vector<std::string> codesOf(const std::vector<rv::catalog::Issue>& issues) {
    std::vector<std::string> codes;
    for (const auto& issue : issues) {
        codes.push_back(issue.code);
    }
    return codes;
}

const SystemDefinition& systemNamed(const SystemsConfiguration& systems, std::string_view name) {
    const SystemDefinition* system = systems.find(name);
    INFO("the system " << name);
    REQUIRE(system != nullptr);
    return *system;
}

std::vector<std::string> namesOf(const std::vector<rv::esde::FindRuleSet>& sets) {
    std::vector<std::string> names;
    for (const auto& set : sets) {
        names.push_back(set.name);
    }
    return names;
}

} // namespace

TEST_CASE("esde/systems: the standard fixture's systems, their commands and the domain's view of them",
          "[esde][systems]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const auto profile = profileOf(fixture);
    const SystemsConfiguration systems = systemsOf(profile);
    CHECK(namesOf(systems.systems) == std::vector<std::string>{"gb", "psx", "snes"});
    CHECK(systems.fallbacks.empty());
    CHECK(systems.bundledFile == fixture.path("Programs/ES-DE/resources/systems/windows/es_systems.xml"));
    CHECK_FALSE(systems.customFile.has_value()); // custom_systems/ is there, empty
    CHECK_FALSE(systems.loadExclusive);
    CHECK(systems.commandCount() == 10);
    CHECK(codesOf(systems.issues) == std::vector<std::string>{"extension-without-dot"}); // psx's "CUE"
    CHECK(systems.issues[0].system == "psx");

    const SystemDefinition& gb = systemNamed(systems, "gb");
    CHECK(gb.fullName == "Nintendo Game Boy");
    CHECK(gb.path == "%ROMPATH%\\gb");
    CHECK(gb.extensions == std::vector<std::string>{".gb", ".GB", ".gbc", ".GBC", ".zip", ".ZIP"});
    CHECK(labelsOf(gb) == std::vector<std::string>{"Fake Emulator", "Fake Emulator (Game Folder)",
                                                   "Missing Emulator (Standalone)", "Home Emulator"});
    REQUIRE(gb.findCommand("Home Emulator") != nullptr);
    CHECK(gb.findCommand("Home Emulator")->text == "%EMULATOR_HOME-EMULATOR% -- %ROM%");
    CHECK(gb.findCommand("home emulator") == nullptr); // labels compare exactly
    CHECK(gb.platforms == std::vector<std::string>{"gb"});
    CHECK(gb.theme == "gb");
    CHECK(gb.origin == ConfigOrigin::Bundled);
    CHECK(systemNamed(systems, "snes").platforms == std::vector<std::string>{"snes", "satellaview"});
    CHECK(systemNamed(systems, "psx").extensions ==
          std::vector<std::string>{".bin", ".BIN", ".chd", ".CHD", ".cue", "CUE", ".m3u", ".M3U"});
    for (const SystemDefinition& system : systems.systems) {
        for (const auto& command : system.commands) {
            CHECK_FALSE(command.placeholder);
        }
    }

    const auto romRoot = romRootOf(profile);
    std::vector<rv::catalog::Issue> issues;
    const std::vector<rv::catalog::SystemInfo> infos = rv::esde::systemInfos(systems, romRoot, profile, issues);
    CHECK(issues.empty());
    REQUIRE(infos.size() == 3);
    const char* const names[] = {"gb", "psx", "snes"};
    for (std::size_t i = 0; i < infos.size(); ++i) {
        INFO(names[i]);
        CHECK(infos[i].name.str() == names[i]);
        CHECK(infos[i].romFolderConfigured == "%ROMPATH%\\" + std::string(names[i]));
        CHECK(infos[i].romFolder == fixture.path("roms") / names[i]);
        CHECK(infos[i].availability == rv::catalog::Availability::Available);
        CHECK(infos[i].unavailableReason == rv::catalog::SourceUnavailableReason::None);
        CHECK(infos[i].gamelist == rv::catalog::GamelistState::Absent);
        CHECK(infos[i].alternativeEmulator.empty());
    }
    CHECK(infos[0].fullName == "Nintendo Game Boy");
    CHECK(infos[2].platforms == std::vector<std::string>{"snes", "satellaview"});
    CHECK(infos[2].theme == "snes");

    // The gamelist ES-DE reads: the ES-DE home's, unless LegacyGamelistFileLocation finds one in the ROM folder.
    auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    CHECK(rv::esde::gamelistFile(infos[0].name, infos[0].romFolder, *settings, profile.esdeHome) ==
          fixture.path("ES-DE/gamelists/gb/gamelist.xml"));
    settings->legacyGamelistFileLocation = true;
    CHECK(rv::esde::gamelistFile(infos[0].name, infos[0].romFolder, *settings, profile.esdeHome) ==
          fixture.path("roms/gb/gamelist.xml"));
    CHECK(rv::esde::gamelistFile(infos[2].name, infos[2].romFolder, *settings, profile.esdeHome) ==
          fixture.path("ES-DE/gamelists/snes/gamelist.xml"));
}

TEST_CASE("esde/systems: custom systems replace bundled ones; ES-DE's label rules and the upstream quirks",
          "[esde][systems]") {
    const rv::test::Fixture fixture = materialize("esde/custom-systems");
    const auto profile = profileOf(fixture);

    SECTION("the custom file first, then the bundled one") {
        const SystemsConfiguration systems = systemsOf(profile);
        CHECK(namesOf(systems.systems) == std::vector<std::string>{"androidgames", "dirgames", "duplabels", "gb",
                                                                   "homebrew", "nes", "pcfx", "unlabelled"});
        CHECK(systems.customFile == fixture.path("ES-DE/custom_systems/es_systems.xml"));
        CHECK_FALSE(systems.loadExclusive);
        // The bundled gb the custom one replaced, the repeated nes: ES-DE falls back to them in this order.
        CHECK(namesOf(systems.fallbacks) == std::vector<std::string>{"gb", "nes"});
        CHECK(systems.fallbacks[0].origin == ConfigOrigin::Bundled);
        CHECK(systems.fallbacks[1].fullName == "Nintendo Entertainment System (again)");
        CHECK(codesOf(systems.issues) ==
              std::vector<std::string>{"platform-missing", "extension-without-dot", "command-label-missing",
                                       "command-label-duplicate", "command-label-missing", "system-skipped",
                                       "system-name-invalid", "system-skipped", "system-replaced",
                                       "system-repeated"});

        const SystemDefinition& gb = systemNamed(systems, "gb");
        CHECK(gb.origin == ConfigOrigin::Custom);
        CHECK(gb.fullName == "Game Boy (custom)");
        CHECK(gb.path == "%ROMPATH%\\gb-custom");
        CHECK(labelsOf(gb) == std::vector<std::string>{"Custom Emulator", "Bundled Emulator"});
        CHECK(systemNamed(systems, "homebrew").origin == ConfigOrigin::Custom);

        const SystemDefinition& nes = systemNamed(systems, "nes");
        CHECK(nes.fullName == "NintendoEntertainment System"); // ES-DE removes line feeds, adds no space
        CHECK(nes.sortName == "Nintendo 1");
        CHECK(nes.extensions == std::vector<std::string>{".nes", ".NES", ".unf", ".UNF", ".zip", ".ZIP"});
        CHECK(nes.platforms == std::vector<std::string>{"nes", "famicom"});
        CHECK(nes.theme == "nes"); // no <theme>: the name
        CHECK(labelsOf(nes) == std::vector<std::string>{"Fake Emulator", "Fake Emulator (Core)", "Shell"});

        const SystemDefinition& dirgames = systemNamed(systems, "dirgames");
        CHECK(dirgames.extensions == std::vector<std::string>{"."});
        CHECK(labelsOf(dirgames) == std::vector<std::string>{""});
        CHECK(dirgames.platforms.empty());
        CHECK(dirgames.theme == "dirgames"); // an empty <theme>: the name
        CHECK(systemNamed(systems, "pcfx").extensions == std::vector<std::string>{".cue", "CUE", ".chd", ".CHD"});
        const SystemDefinition& android = systemNamed(systems, "androidgames");
        REQUIRE(android.commands.size() == 1);
        CHECK(android.commands[0].placeholder);
        CHECK(android.commands[0].label.empty());

        // A labelled command after an unlabelled first one, a repeated label, an unlabelled one among labelled ones.
        const SystemDefinition& unlabelled = systemNamed(systems, "unlabelled");
        REQUIRE(unlabelled.commands.size() == 1);
        CHECK(unlabelled.commands[0].text == "%EMULATOR_FAKE-EMULATOR% --first %ROM%");
        const SystemDefinition& duplabels = systemNamed(systems, "duplabels");
        CHECK(labelsOf(duplabels) == std::vector<std::string>{"Same", "Other"});
        CHECK(duplabels.commands[0].text == "%EMULATOR_FAKE-EMULATOR% --one %ROM%");
        CHECK(systems.find("nofullname") == nullptr);
        CHECK(systems.find("bad:name") == nullptr);

        const auto romRoot = romRootOf(profile);
        std::vector<rv::catalog::Issue> issues;
        const auto infos = rv::esde::systemInfos(systems, romRoot, profile, issues);
        CHECK(issues.empty());
        const auto info = [&](std::string_view name) {
            const auto found = std::find_if(infos.begin(), infos.end(), [&](const rv::catalog::SystemInfo& system) {
                return system.name.str() == name;
            });
            REQUIRE(found != infos.end());
            return *found;
        };
        CHECK(info("gb").romFolder == fixture.path("roms/gb-custom"));
        CHECK(info("gb").fullName == "Game Boy (custom)");
        CHECK(info("homebrew").availability == rv::catalog::Availability::Available);
        CHECK(info("androidgames").availability == rv::catalog::Availability::SourceUnavailable);
        CHECK(info("androidgames").unavailableReason == rv::catalog::SourceUnavailableReason::FolderMissing);
    }
    SECTION("<loadExclusive/> in the custom file: only the custom systems") {
        REQUIRE(fixture.replaceText("ES-DE/custom_systems/es_systems.xml", "<!-- LOAD-EXCLUSIVE -->",
                                    "<loadExclusive/>"));
        // The bundled file is not even read: a broken one does not matter.
        REQUIRE(fixture.replaceText("Programs/ES-DE/resources/systems/windows/es_systems.xml", "</systemList>", ""));
        const SystemsConfiguration systems = systemsOf(profile);
        CHECK(systems.loadExclusive);
        CHECK(namesOf(systems.systems) == std::vector<std::string>{"gb", "homebrew"});
        CHECK(systems.fallbacks.empty());
        CHECK(systems.issues.empty());
    }
    SECTION("<loadExclusive/> in the bundled file is ignored") {
        REQUIRE(fixture.replaceText("Programs/ES-DE/resources/systems/windows/es_systems.xml",
                                    "<!-- BUNDLED-LOAD-EXCLUSIVE -->", "<loadExclusive/>"));
        const SystemsConfiguration systems = systemsOf(profile);
        CHECK_FALSE(systems.loadExclusive);
        CHECK(systems.systems.size() == 8);
        const std::vector<std::string> codes = codesOf(systems.issues);
        CHECK(std::count(codes.begin(), codes.end(), "load-exclusive-ignored") == 1);
    }
    SECTION("files ES-DE refuses to start with are errors") {
        REQUIRE(fixture.replaceText("ES-DE/custom_systems/es_systems.xml", "</systemList>", ""));
        const auto truncated = rv::esde::loadSystems(profile);
        REQUIRE_FALSE(truncated.has_value());
        CHECK(truncated.error().code == rv::ErrorCode::ParseError);
        CHECK(rv::core::pathFromUtf8(truncated.error().context) ==
              fixture.path("ES-DE/custom_systems/es_systems.xml"));

        REQUIRE(rv::core::writeTextFileAtomic(fixture.path("ES-DE/custom_systems/es_systems.xml"),
                                              "<?xml version=\"1.0\"?>\n<systems/>\n")
                    .has_value());
        const auto noList = rv::esde::loadSystems(profile);
        REQUIRE_FALSE(noList.has_value());
        CHECK(noList.error().code == rv::ErrorCode::ParseError);

        rv::catalog::LibraryProfile noInstall = profile;
        noInstall.installFolder.reset();
        const auto noBundled = rv::esde::loadSystems(noInstall);
        REQUIRE_FALSE(noBundled.has_value());
        CHECK(noBundled.error().code == rv::ErrorCode::NotFound);
    }
}

TEST_CASE("esde/systems: ROM folders built as ES-DE builds them, from %ROMPATH%, '~' and absolute paths",
          "[esde][systems]") {
    const rv::test::Fixture fixture = materialize("esde/custom-dirs");
    const auto profile = profileOf(fixture);
    const SystemsConfiguration systems = systemsOf(profile);
    const auto romRoot = romRootOf(profile);
    std::vector<rv::catalog::Issue> issues;
    const auto infos = rv::esde::systemInfos(systems, romRoot, profile, issues);
    CHECK(issues.empty());
    REQUIRE(infos.size() == 5);
    struct Expected {
        std::string_view name;
        fs::path folder;
        bool available;
    };
    const Expected expected[] = {
        {"gb", fixture.path("Games/ROM Library/gb"), true},
        {"n64", fixture.path("Games/ROM Library/n64"), false},
        {"pico8", fixture.path("Homebrew/pico8"), true},
        {"snes", fixture.path("Games/ROM Library/snes"), true}, // "%ROMPATH%/snes"
        {"vectrex", fixture.path("Elsewhere/vectrex"), true},
    };
    for (std::size_t i = 0; i < infos.size(); ++i) {
        INFO(expected[i].name);
        CHECK(infos[i].name.str() == expected[i].name);
        CHECK(infos[i].romFolder == expected[i].folder);
        CHECK((infos[i].availability == rv::catalog::Availability::Available) == expected[i].available);
    }

    // A ROM root that cannot be resolved leaves the %ROMPATH% systems without a folder, reported once by the root.
    rv::esde::ResolvedRoot unresolved;
    std::vector<rv::catalog::Issue> none;
    const auto withoutRoot = rv::esde::systemInfos(systems, unresolved, profile, none);
    CHECK(none.empty());
    CHECK(withoutRoot[0].romFolder.empty());
    CHECK(withoutRoot[0].unavailableReason == rv::catalog::SourceUnavailableReason::FolderMissing);
    CHECK(withoutRoot[2].romFolder == fixture.path("Homebrew/pico8"));

    // A relative path names no folder on its own.
    REQUIRE(fixture.replaceText("Programs/ES-DE/resources/systems/windows/es_systems.xml", "<path>~\\Homebrew\\pico8",
                                "<path>Homebrew\\pico8"));
    std::vector<rv::catalog::Issue> relative;
    const auto relativeInfos = rv::esde::systemInfos(systemsOf(profile), romRoot, profile, relative);
    CHECK(codesOf(relative) == std::vector<std::string>{"rom-folder-unresolved"});
    CHECK(relative[0].system == "pico8");
    CHECK(relativeInfos[2].availability == rv::catalog::Availability::SourceUnavailable);
}

TEST_CASE("esde/find_rules: the standard fixture's emulators and core, their rules typed and in order",
          "[esde][find_rules]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const rv::esde::FindRules rules = rv::esde::loadFindRules(profileOf(fixture));
    CHECK(namesOf(rules.emulators) ==
          std::vector<std::string>{"FAKE-EMULATOR", "HOME-EMULATOR", "MISSING-EMULATOR", "OS-SHELL"});
    CHECK(namesOf(rules.cores) == std::vector<std::string>{"FAKE-EMULATOR"});
    CHECK(rules.files ==
          std::vector<fs::path>{fixture.path("Programs/ES-DE/resources/systems/windows/es_find_rules.xml")});
    CHECK(rules.issues.empty());
    CHECK(rules.entryCount() == 10);

    const rv::esde::FindRuleSet* fake = rules.findEmulator("FAKE-EMULATOR");
    REQUIRE(fake != nullptr);
    CHECK(fake->origin == ConfigOrigin::Bundled);
    REQUIRE(fake->rules.size() == 3);
    CHECK(fake->rules[0].type == FindRuleType::WinRegistryPath);
    CHECK(fake->rules[1].type == FindRuleType::SystemPath);
    CHECK(fake->rules[2].type == FindRuleType::StaticPath);
    CHECK(fake->entries(FindRuleType::StaticPath) ==
          std::vector<std::string>{"~\\AppData\\Roaming\\FakeEmulator\\fake-emulator.exe",
                                   "%ESPATH%\\Emulators\\FakeEmulator\\fake-emulator.exe",
                                   "%ESPATH%\\..\\Emulators\\FakeEmulator\\fake-emulator.exe"});
    CHECK(fake->entries(FindRuleType::WinRegistryValue).empty());
    REQUIRE(rules.findCore("FAKE-EMULATOR") != nullptr);
    CHECK(rules.findCore("FAKE-EMULATOR")->entries(FindRuleType::CorePath) ==
          std::vector<std::string>{"%EMUPATH%\\cores"});
    CHECK(rules.findEmulator("fake-emulator") == nullptr); // names compare exactly
    CHECK(rules.findCore("OS-SHELL") == nullptr);
    CHECK(rv::esde::parseFindRuleType("staticpath") == FindRuleType::StaticPath);
    CHECK_FALSE(rv::esde::parseFindRuleType("StaticPath").has_value());
    CHECK(rv::esde::toString(FindRuleType::WinRegistryValue) == "winregistryvalue");
}

TEST_CASE("esde/find_rules: custom find rules replace the bundled ones; what ES-DE skips is reported",
          "[esde][find_rules]") {
    const rv::test::Fixture fixture = materialize("esde/custom-systems");
    const auto profile = profileOf(fixture);

    const rv::esde::FindRules rules = rv::esde::loadFindRules(profile);
    CHECK(namesOf(rules.emulators) ==
          std::vector<std::string>{"CUSTOM-EMULATOR", "FAKE-EMULATOR", "ODD-RULES", "OS-SHELL"});
    CHECK(namesOf(rules.cores) == std::vector<std::string>{"ODD-CORE", "RETROARCH"});
    CHECK(rules.files == std::vector<fs::path>{fixture.path("ES-DE/custom_systems/es_find_rules.xml"),
                                               fixture.path("Programs/ES-DE/resources/systems/windows/"
                                                            "es_find_rules.xml")});
    CHECK(codesOf(rules.issues) ==
          std::vector<std::string>{"find-rule-repeated", "find-rule-skipped", "find-rule-skipped", "find-rule-skipped",
                                   "find-rule-skipped", "find-rule-replaced", "find-rule-replaced",
                                   "find-rule-skipped"});

    const rv::esde::FindRuleSet* fake = rules.findEmulator("FAKE-EMULATOR");
    REQUIRE(fake != nullptr);
    CHECK(fake->origin == ConfigOrigin::Custom);
    CHECK(fake->entries(FindRuleType::StaticPath) ==
          std::vector<std::string>{"~\\Custom Emulators\\fake-emulator.exe"});
    const rv::esde::FindRuleSet* custom = rules.findEmulator("CUSTOM-EMULATOR");
    REQUIRE(custom != nullptr);
    std::vector<FindRuleType> types;
    for (const auto& rule : custom->rules) {
        types.push_back(rule.type);
    }
    CHECK(types == std::vector<FindRuleType>{FindRuleType::WinRegistryValue, FindRuleType::SystemPath,
                                             FindRuleType::StaticPath, FindRuleType::WinRegistryPath});
    CHECK(custom->entries(FindRuleType::StaticPath) ==
          std::vector<std::string>{"%ESPATH%\\Emulators\\Custom\\custom-emulator-*.exe",
                                   "%ROMPATH%\\..\\Emulators\\custom-emulator.exe|\"C:\\Tools\\launcher.exe\" --run"});
    CHECK(custom->entries(FindRuleType::WinRegistryValue) ==
          std::vector<std::string>{"SOFTWARE\\Custom Emulator\\InstallDir|\\custom-emulator.exe"});
    const rv::esde::FindRuleSet* odd = rules.findEmulator("ODD-RULES");
    REQUIRE(odd != nullptr);
    REQUIRE(odd->rules.size() == 1);
    CHECK(odd->rules[0].entries == std::vector<std::string>{" spaced entry.exe "}); // as written, not trimmed
    CHECK(rules.findEmulator("OS-SHELL")->origin == ConfigOrigin::Bundled);
    CHECK(rules.findCore("RETROARCH")->origin == ConfigOrigin::Custom);
    CHECK(rules.findCore("RETROARCH")->entries(FindRuleType::CorePath) ==
          std::vector<std::string>{"~\\Custom Cores"});
    CHECK(rules.findCore("ODD-CORE")->rules.empty());

    // A custom file that cannot be parsed is skipped, as ES-DE skips it; the bundled rules still apply.
    REQUIRE(fixture.replaceText("ES-DE/custom_systems/es_find_rules.xml", "</ruleList>", ""));
    const rv::esde::FindRules broken = rv::esde::loadFindRules(profile);
    CHECK(codesOf(broken.issues).front() == "find-rules-unreadable");
    CHECK(namesOf(broken.emulators) == std::vector<std::string>{"FAKE-EMULATOR", "OS-SHELL"});
    CHECK(broken.findEmulator("FAKE-EMULATOR")->origin == ConfigOrigin::Bundled);

    // No bundled file at all.
    rv::catalog::LibraryProfile noInstall = profile;
    noInstall.installFolder.reset();
    CHECK(codesOf(rv::esde::loadFindRules(noInstall).issues).back() == "find-rules-missing");
}
