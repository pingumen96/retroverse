#pragma once

// I2 step 10's tests of the ES-DE launch translation: a machine in a temporary folder, described to the translation by
// rv::test::FakeSystemProbe. Programs are PE stubs on disk (launcher::validateLaunchRequest reads their headers) that
// the probe lists; ROMs and %INJECT% files are real files; cores need only be listed. Find rules and systems are built
// in code, as the translation receives them from es_find_rules.xml and es_systems.xml.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/launch_resolution.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/launch_translation.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>

#include "support/fake_system_probe.hpp"
#include "support/program_stub.hpp"
#include "support/temp_dir.hpp"

namespace rv::test {

class LaunchMachine {
public:
    LaunchMachine() {
        // ~ is ES-DE's home folder, the ES-DE home's parent.
        profile.esdeHome = home() / "ES-DE";
        profile.installFolder = install();
        romRoot.path = roms();
        romRoot.source = esde::RootSource::Setting;
        romRoot.configured = core::pathToUtf8(roms());
        romRoot.esdeText = core::pathToUtf8(roms()) + "\\";
        probe.setHomeFolder(home())
            .setSystem32Folder(root() / "Windows" / "System32")
            .setWindowsFolder(root() / "Windows");
    }

    [[nodiscard]] std::filesystem::path root() const { return m_dir.path(); }
    [[nodiscard]] std::filesystem::path home() const { return root() / "Users" / "Player"; }
    [[nodiscard]] std::filesystem::path install() const { return root() / "ES-DE"; }
    [[nodiscard]] std::filesystem::path roms() const { return root() / "ROMs"; }

    /// A program stub at `path` (absolute), listed by the probe.
    std::filesystem::path program(const std::filesystem::path& path) {
        writeProgramStub(path);
        probe.addFile(path);
        return path;
    }
    /// A file the probe lists, not written (a core).
    std::filesystem::path listed(const std::filesystem::path& path) {
        probe.addFile(path);
        return path;
    }
    /// A real file with `content` (a ROM, an %INJECT% file).
    std::filesystem::path file(const std::filesystem::path& path, std::string_view content = "rom") {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary).write(content.data(), static_cast<std::streamsize>(content.size()));
        return path;
    }

    /// Adds an emulator's rules (type, entries), in the order given.
    void emulator(std::string name, std::vector<esde::FindRule> rules) {
        rulesOf(findRules.emulators, std::move(name), std::move(rules));
    }
    void core(std::string name, std::vector<std::string> corePaths) {
        rulesOf(findRules.cores, std::move(name), {esde::FindRule{esde::FindRuleType::CorePath, std::move(corePaths)}});
    }

    [[nodiscard]] esde::TranslationContext context() const {
        return esde::TranslationContext{profile, romRoot, &findRules, &probe, nullptr, &protocols};
    }

    catalog::LibraryProfile profile;
    /// The protocols an Internet Shortcut may activate (I11 step 13): none unless a test adds them.
    std::vector<launcher::ProtocolRule> protocols;
    esde::ResolvedRoot romRoot;
    esde::FindRules findRules;
    FakeSystemProbe probe;

private:
    static void rulesOf(std::vector<esde::FindRuleSet>& sets, std::string name, std::vector<esde::FindRule> rules) {
        sets.push_back(esde::FindRuleSet{std::move(name), esde::ConfigOrigin::Bundled, std::move(rules)});
        std::sort(sets.begin(), sets.end(),
                  [](const esde::FindRuleSet& a, const esde::FindRuleSet& b) { return a.name < b.name; });
    }

    TempDir m_dir{"rv-launch"};
};

/// A system "gb" (or `name`) whose ROM folder is %ROMPATH%\<name>, with the commands given as (label, text).
inline esde::SystemDefinition testSystem(std::vector<std::pair<std::string, std::string>> commands,
                                         std::string_view name = "gb") {
    esde::SystemDefinition system;
    system.name = *catalog::SystemName::parse(name);
    system.fullName = "Test system";
    system.path = "%ROMPATH%\\" + std::string(name);
    system.extensions = {".gb"};
    for (auto& [label, text] : commands) {
        esde::SystemCommand command;
        command.label = std::move(label);
        command.text = std::move(text);
        system.commands.push_back(std::move(command));
    }
    system.platforms = {std::string(name)};
    system.theme = std::string(name);
    return system;
}

/// A game of a system, by its relative path as listed on disk.
inline catalog::LaunchGame testGame(std::string relativePath, bool directory = false) {
    catalog::LaunchGame game;
    game.relativePath = std::move(relativePath);
    game.directory = directory;
    return game;
}

/// The codes of a translation's blockers, in order.
inline std::vector<std::string> blockerCodes(const catalog::LaunchTranslation& translation) {
    std::vector<std::string> codes;
    for (const catalog::ReadinessBlocker& blocker : translation.blockers) {
        codes.push_back(blocker.code);
    }
    return codes;
}

/// Every blocker message joined, for INFO() and substring checks.
inline std::string blockerText(const catalog::LaunchTranslation& translation) {
    std::string text;
    for (const catalog::ReadinessBlocker& blocker : translation.blockers) {
        text += blocker.code + ": " + blocker.message + "\n";
    }
    return text;
}

} // namespace rv::test
