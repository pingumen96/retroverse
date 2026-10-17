#pragma once

// Private to rv_cli_core (I2 step 10): what `profiles`, `launch` and `assign` share: the library they translate commands
// for (library.json, its settings, systems and find rules, the gamelists' alternative emulators), the emulator
// profiles, Retroverse's choices for the systems (the catalog database's, from `assign`, rank before emulators.json's:
// I2 step 12), the machine (Win32SystemProbe), and how choices, translations and blockers read as text and as JSON.

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog/system.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/paths.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/gamelist.hpp>
#include <rv/esde/launch_translation.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>
#include <rv/launcher/emulator_profile.hpp>
#include <rv/launcher/launch_request.hpp>
#include <rv/platform_win/system_probe.hpp>

#include "esde_text.hpp"

namespace rv::cli {

/// Everything a launch is resolved with. Held by pointer: the translator and the context point into it.
struct LaunchLibrary {
    catalog::LibraryProfile profile;
    esde::EsdeSettings settings;
    esde::ResolvedRoot romRoot;
    esde::SystemsConfiguration systems;
    esde::FindRules findRules;
    std::vector<catalog::SystemInfo> systemInfos;
    launcher::EmulatorProfiles profiles;
    platform_win::Win32SystemProbe probe;
    std::unique_ptr<esde::EsdeCommandTranslator> translator;
    /// Retroverse's choices for the library's systems that the catalog database holds (retroverse-cli assign), by
    /// system name: they rank before emulators.json's "systemChoices" (systemChoices()). Empty until
    /// loadAssignedChoices() or the launch's own read of the database.
    std::map<std::string, launcher::LaunchChoice, std::less<>> assignedChoices;
    /// What reading the library met: settings, roots, systems, find rules, gamelists, profiles.
    std::vector<catalog::Issue> issues;

    [[nodiscard]] catalog::LaunchContext context() const {
        return catalog::LaunchContext{translator.get(), &profiles, &probe, nullptr};
    }
    [[nodiscard]] const catalog::SystemInfo* findSystem(std::string_view name) const {
        const auto found = std::find_if(systemInfos.begin(), systemInfos.end(),
                                        [&](const catalog::SystemInfo& system) { return system.name.str() == name; });
        return found == systemInfos.end() ? nullptr : &*found;
    }
};

/// Reads the library <user data>/config/library.json records and the emulator profiles. On failure the message goes to
/// the context's stderr and `failure` says how the command exits: UsageError without a library.json (or with one that
/// is not valid: library create writes it), RuntimeFailure when ES-DE's settings or systems cannot be read.
inline std::unique_ptr<LaunchLibrary> loadLaunchLibrary(CommandContext& context, std::string_view name,
                                                        ExitCode& failure) {
    const std::filesystem::path file = context.paths.configDir() / esde::kLibraryFileName;
    auto profile = esde::readLibraryFile(file);
    if (!profile) {
        context.err.writeLine(fmt::format("{}: no library: {} (run 'retroverse-cli library create' first)", name,
                                          profile.error().toString()));
        failure = ExitCode::UsageError;
        return nullptr;
    }
    auto library = std::make_unique<LaunchLibrary>();
    library->profile = std::move(*profile);
    auto settings = esde::readSettings(library->profile.esdeHome);
    if (!settings) {
        context.err.writeLine(fmt::format("{}: ES-DE's settings cannot be read: {}", name, settings.error().toString()));
        failure = ExitCode::RuntimeFailure;
        return nullptr;
    }
    library->settings = std::move(*settings);
    library->romRoot = esde::resolveRomRoot(library->settings, library->profile);
    auto systems = esde::loadSystems(library->profile);
    if (!systems) {
        context.err.writeLine(fmt::format("{}: ES-DE's systems cannot be read: {}", name, systems.error().toString()));
        failure = ExitCode::RuntimeFailure;
        return nullptr;
    }
    library->systems = std::move(*systems);
    library->findRules = esde::loadFindRules(library->profile);
    auto& issues = library->issues;
    issues.insert(issues.end(), library->settings.issues.begin(), library->settings.issues.end());
    issues.insert(issues.end(), library->romRoot.issues.begin(), library->romRoot.issues.end());
    issues.insert(issues.end(), library->systems.issues.begin(), library->systems.issues.end());
    issues.insert(issues.end(), library->findRules.issues.begin(), library->findRules.issues.end());
    library->systemInfos = esde::systemInfos(library->systems, library->romRoot, library->profile, issues);
    library->profiles = launcher::loadEmulatorProfiles(context.paths);
    for (const launcher::ProfileIssue& issue : library->profiles.issues) {
        issues.push_back(catalog::Issue{catalog::IssueSeverity::Warning, "profile-issue", {}, issue.file,
                                        issue.toString()});
    }
    library->translator = std::make_unique<esde::EsdeCommandTranslator>(
        esde::TranslationContext{library->profile, library->romRoot, &library->findRules, &library->probe},
        library->systems);
    return library;
}

/// The gamelist of `system`, when it has one that can be read: what gives its <alternativeEmulator> and the games'
/// <altemulator>. A gamelist that fails is an issue (the choices it holds are then unknown).
inline std::optional<esde::Gamelist> readSystemGamelist(LaunchLibrary& library, const catalog::SystemInfo& system) {
    if (system.romFolder.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path file =
        esde::gamelistFile(system.name, system.romFolder, library.settings, library.profile.esdeHome);
    auto gamelist = esde::readGamelist(file, system.name, system.romFolder);
    if (!gamelist) {
        if (gamelist.error().code != ErrorCode::NotFound) {
            library.issues.push_back(catalog::Issue{
                catalog::IssueSeverity::Warning, "gamelist-unreadable", system.name.str(), pathText(file),
                fmt::format("the gamelist cannot be read ({}): its alternative emulators are not known",
                            gamelist.error().message)});
        }
        return std::nullopt;
    }
    return std::move(*gamelist);
}

/// Where Retroverse's choice for a system comes from (I2 step 12): the catalog database's (`assign`), which ranks first
/// as the user's latest explicit choice, else emulators.json's "systemChoices".
inline constexpr std::string_view kAssignedOrigin = "assign";
inline constexpr std::string_view kEmulatorsJsonOrigin = "emulators.json";

/// Where Retroverse's choice for `system` comes from: kAssignedOrigin, kEmulatorsJsonOrigin, or empty when it has none.
inline std::string_view systemChoiceOrigin(const LaunchLibrary& library, std::string_view system) {
    if (library.assignedChoices.find(system) != library.assignedChoices.end()) {
        return kAssignedOrigin;
    }
    return library.profiles.systemChoice(system) != nullptr ? kEmulatorsJsonOrigin : std::string_view();
}

/// The choices of a system without a game: Retroverse's for the system (the catalog database's from `assign`, else
/// emulators.json's), the gamelist's <alternativeEmulator>.
inline catalog::LaunchChoices systemChoices(const LaunchLibrary& library, const catalog::SystemInfo& system,
                                            const std::string& alternativeEmulator) {
    catalog::LaunchChoices choices;
    choices.alternativeEmulatorPerGame = library.settings.alternativeEmulatorPerGame;
    if (const auto assigned = library.assignedChoices.find(system.name.str());
        assigned != library.assignedChoices.end()) {
        choices.system = assigned->second;
    } else if (const launcher::LaunchChoice* choice = library.profiles.systemChoice(system.name.str())) {
        choices.system = *choice;
    }
    choices.systemAlternativeEmulator = alternativeEmulator;
    return choices;
}

/// Reads into `library` Retroverse's choices for its systems from the catalog database (retroverse-cli assign), opened
/// for reading only: never made, migrated or written. No database yet, or one that holds no choice of the library: no
/// choices. A database that cannot be read is an issue (assign's choices are then not known).
inline void loadAssignedChoices(LaunchLibrary& library, const core::Paths& paths) {
    catalog_db::DatabaseOptions options;
    options.create = false;
    options.switchToWal = false; // reading writes nothing into the file
    const auto unknown = [&](const Error& error) {
        library.issues.push_back(catalog::Issue{
            catalog::IssueSeverity::Warning, "catalog-unreadable", {}, pathText(catalog_db::databaseFile(paths)),
            fmt::format("the catalog database cannot be read ({}): the choices made with 'retroverse-cli assign' are "
                        "not known",
                        error.message)});
    };
    auto database = catalog_db::Database::open(catalog_db::databaseFile(paths), options);
    if (!database) {
        if (database.error().code != ErrorCode::NotFound) {
            unknown(database.error());
        }
        return;
    }
    auto reader = database->openReader();
    if (!reader) {
        unknown(reader.error());
        return;
    }
    auto store = catalog_db::SessionStore::open(*reader);
    if (!store) {
        unknown(store.error());
        return;
    }
    auto choices = (*store)->systemChoices(library.profile.id);
    if (!choices) {
        unknown(choices.error());
        return;
    }
    for (catalog_db::SystemChoiceRow& row : *choices) {
        library.assignedChoices.emplace(row.system.str(), std::move(row.stored.choice));
    }
}

/// "command \"bsnes\"", "profile 'retroarch'", "the command" (a single unlabelled one).
inline std::string choiceText(const launcher::LaunchChoice& choice) {
    if (choice.kind == launcher::LaunchChoiceKind::Profile) {
        return fmt::format("profile '{}'", choice.value);
    }
    return choice.value.empty() ? std::string("the command") : fmt::format("command \"{}\"", choice.value);
}

inline Json choiceJson(const launcher::LaunchChoice& choice) {
    return Json{{"kind", std::string(launcher::toString(choice.kind))}, {"value", choice.value}};
}

/// Why a choice was made, for people: the source, with where Retroverse's choice came from ("Retroverse's choice for the
/// system (assign)") when `origin` says it.
inline std::string whyText(const catalog::ChosenLaunch& chosen, std::string_view origin = {}) {
    const bool retroverse = chosen.source == catalog::ChoiceSource::RetroverseGame ||
                            chosen.source == catalog::ChoiceSource::RetroverseSystem;
    if (!retroverse || origin.empty()) {
        return std::string(catalog::describe(chosen.source));
    }
    return fmt::format("{} ({})", catalog::describe(chosen.source), origin);
}

/// `origin`: where Retroverse's choice came from (kAssignedOrigin, kEmulatorsJsonOrigin), for a Retroverse choice; it
/// becomes "origin" (null otherwise) and part of "why".
inline Json chosenJson(const catalog::ChosenLaunch& chosen, std::string_view origin = {}) {
    Json json = choiceJson(chosen.choice);
    json["source"] = std::string(catalog::toString(chosen.source));
    const bool retroverse = chosen.source == catalog::ChoiceSource::RetroverseGame ||
                            chosen.source == catalog::ChoiceSource::RetroverseSystem;
    json["origin"] = retroverse && !origin.empty() ? Json(std::string(origin)) : Json(nullptr);
    json["why"] = whyText(chosen, origin);
    json["warnings"] = chosen.warnings;
    return json;
}

/// "<path> (winregistrypath \"retroarch.exe\", HKLM)".
inline std::string foundText(const catalog::FoundLocation& found) {
    return fmt::format("{} ({} \"{}\"{})", pathText(found.path), found.rule, found.entry,
                       found.detail.empty() ? std::string() : ", " + found.detail);
}

inline Json foundJson(const std::optional<catalog::FoundLocation>& found) {
    if (!found) {
        return nullptr;
    }
    return Json{
        {"path", pathText(found->path)}, {"rule", found->rule}, {"entry", found->entry}, {"detail", found->detail}};
}

inline Json blockersJson(const std::vector<catalog::ReadinessBlocker>& blockers) {
    Json json = Json::array();
    for (const catalog::ReadinessBlocker& blocker : blockers) {
        json.push_back(Json{{"readiness", std::string(catalog::toString(blocker.readiness))},
                            {"code", blocker.code},
                            {"message", blocker.message}});
    }
    return json;
}

/// A translation's readiness, program, core, blockers, attempts and notes as JSON.
inline Json translationJson(const catalog::LaunchTranslation& translation) {
    return Json{{"readiness", std::string(catalog::toString(translation.readiness()))},
                {"program", foundJson(translation.program)},
                {"core", foundJson(translation.core)},
                {"blockers", blockersJson(translation.blockers)},
                {"attempts", translation.attempts},
                {"notes", translation.notes}};
}

/// The lines of a translation's details, "<label><text>", the labels `width` wide after `indent`; `capitalized`: the
/// labels begin with a capital ("Program"), as the lines around them do.
inline void writeTranslationDetails(Output& out, const catalog::LaunchTranslation& translation,
                                    std::string_view indent, std::size_t width, bool capitalized) {
    const auto line = [&](std::string_view label, const std::string& text) {
        std::string name(label);
        if (capitalized && !name.empty()) {
            name.front() = static_cast<char>(name.front() - 'a' + 'A');
        }
        out.writeLine(fmt::format("{}{:<{}}{}", indent, name, width, text));
    };
    if (translation.program) {
        line("program", foundText(*translation.program));
    }
    if (translation.core) {
        line("core", foundText(*translation.core));
    }
    for (const catalog::ReadinessBlocker& blocker : translation.blockers) {
        line("blocked", fmt::format("{} ({}): {}", catalog::describe(blocker.readiness), blocker.code,
                                    blocker.message));
    }
    if (translation.readiness() == catalog::LaunchReadiness::EmulatorNotFound ||
        translation.readiness() == catalog::LaunchReadiness::CoreNotFound) {
        for (const std::string& attempt : translation.attempts) {
            line("tried", attempt);
        }
    }
    for (const std::string& note : translation.notes) {
        line("note", note);
    }
}

/// Writes `text` or `json` where the command's --output says, then finishes the file. False when it cannot be written
/// (the message on stderr).
inline bool finishOutput(CommandContext& context, std::optional<Output>& file, std::string_view name) {
    if (file) {
        if (const Status written = file->finish(); !written) {
            context.err.writeLine(fmt::format("{}: {}", name, written.error().toString()));
            return false;
        }
    }
    return true;
}

} // namespace rv::cli
