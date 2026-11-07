#include <rv/library/launch_library.hpp>

#include <algorithm>
#include <filesystem>
#include <utility>

#include <fmt/format.h>

#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/utf8.hpp>

namespace rv::library {
namespace {

/// The game's availability on disk now: its system's folder, then each element of its path as the folder spells it
/// (as retroverse-cli's launch --game finds it).
void findOnDisk(const catalog::SystemInfo& system, catalog::LaunchGame& game) {
    if (system.availability != catalog::Availability::Available) {
        game.availability = catalog::Availability::SourceUnavailable;
        game.unavailableReason = system.unavailableReason;
        return;
    }
    const auto listed = esde::listedPath(system.romFolder, game.relativePath);
    if (listed) {
        game.relativePath = listed->relativePath;
        game.directory = listed->directory;
        game.availability = catalog::Availability::Available;
        return;
    }
    switch (listed.error().code) {
    case ErrorCode::InvalidArgument: // a stored path no file can have now: the game cannot be there
    case ErrorCode::NotFound:
        game.availability = catalog::Availability::Missing;
        break;
    case ErrorCode::Unavailable:
        game.availability = catalog::Availability::SourceUnavailable;
        game.unavailableReason = catalog::SourceUnavailableReason::Unreachable;
        break;
    default:
        game.availability = catalog::Availability::SourceUnavailable;
        game.unavailableReason = catalog::SourceUnavailableReason::NotListable;
        break;
    }
}

/// A system the library no longer configures: nothing to translate.
catalog::LaunchResolution notConfigured(const catalog::SystemName& system,
                                        const std::optional<launcher::LaunchChoice>& gameChoice) {
    catalog::LaunchResolution resolution;
    resolution.chosen.choice = gameChoice.value_or(launcher::LaunchChoice{});
    resolution.chosen.source =
        gameChoice ? catalog::ChoiceSource::RetroverseGame : catalog::ChoiceSource::FirstCommand;
    resolution.translation.blockers.push_back(catalog::ReadinessBlocker{
        catalog::LaunchReadiness::RequiresConfiguration, "system-not-configured",
        fmt::format("the library no longer configures the system {} (es_systems.xml)", system.str())});
    return resolution;
}

} // namespace

const catalog::SystemInfo* LaunchLibrary::findSystem(std::string_view name) const {
    const auto found = std::find_if(systemInfos.begin(), systemInfos.end(),
                                    [&](const catalog::SystemInfo& system) { return system.name.str() == name; });
    return found == systemInfos.end() ? nullptr : &*found;
}

Expected<std::unique_ptr<LaunchLibrary>> loadLaunchLibrary(catalog::LibraryProfile profile, const core::Paths& paths) {
    auto library = std::make_unique<LaunchLibrary>();
    library->profile = std::move(profile);
    const auto failed = [](std::string_view what, const Error& error) {
        return Error{error.code, fmt::format("ES-DE's {} cannot be read: {}", what, error.toString()), {}};
    };
    auto settings = esde::readSettings(library->profile.esdeHome);
    if (!settings) {
        return propagate(failed("settings", settings.error()));
    }
    library->settings = std::move(*settings);
    library->romRoot = esde::resolveRomRoot(library->settings, library->profile);
    auto systems = esde::loadSystems(library->profile);
    if (!systems) {
        return propagate(failed("systems", systems.error()));
    }
    library->systems = std::move(*systems);
    library->findRules = esde::loadFindRules(library->profile);
    auto& issues = library->issues;
    issues.insert(issues.end(), library->settings.issues.begin(), library->settings.issues.end());
    issues.insert(issues.end(), library->romRoot.issues.begin(), library->romRoot.issues.end());
    issues.insert(issues.end(), library->systems.issues.begin(), library->systems.issues.end());
    issues.insert(issues.end(), library->findRules.issues.begin(), library->findRules.issues.end());
    library->systemInfos = esde::systemInfos(library->systems, library->romRoot, library->profile, issues);
    library->profiles = launcher::loadEmulatorProfiles(paths);
    for (const launcher::ProfileIssue& issue : library->profiles.issues) {
        issues.push_back(catalog::Issue{catalog::IssueSeverity::Warning, "profile-issue", {}, issue.file,
                                        issue.toString()});
    }
    library->translator = std::make_unique<esde::EsdeCommandTranslator>(
        esde::TranslationContext{library->profile, library->romRoot, &library->findRules, &library->probe,
                                 &library->programChoices, &library->profiles.protocols},
        library->systems);
    return library;
}

std::optional<esde::Gamelist> readSystemGamelist(LaunchLibrary& library, const catalog::SystemInfo& system) {
    if (system.romFolder.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path file =
        esde::gamelistFile(system.name, system.romFolder, library.settings, library.profile.esdeHome);
    auto gamelist = esde::readGamelist(file, system.name, system.romFolder);
    if (!gamelist) {
        if (gamelist.error().code != ErrorCode::NotFound) {
            library.issues.push_back(catalog::Issue{
                catalog::IssueSeverity::Warning, "gamelist-unreadable", system.name.str(), core::pathToUtf8Lossy(file),
                fmt::format("the gamelist cannot be read ({}): its alternative emulators are not known",
                            gamelist.error().message)});
        }
        return std::nullopt;
    }
    return std::move(*gamelist);
}

catalog::LaunchChoices launchChoices(const LaunchLibrary& library, const catalog::SystemName& system,
                                     const std::optional<launcher::LaunchChoice>& game, std::string gameAltEmulator,
                                     const std::optional<launcher::LaunchChoice>& assignedSystem,
                                     std::string systemAlternativeEmulator) {
    catalog::LaunchChoices choices;
    choices.alternativeEmulatorPerGame = library.settings.alternativeEmulatorPerGame;
    choices.game = game;
    choices.gameAltEmulator = std::move(gameAltEmulator);
    // assign's choice ranks before emulators.json's for the same system: the user's latest explicit choice.
    if (assignedSystem) {
        choices.system = assignedSystem;
    } else if (const launcher::LaunchChoice* choice = library.profiles.systemChoice(system.str())) {
        choices.system = *choice;
    }
    choices.systemAlternativeEmulator = std::move(systemAlternativeEmulator);
    return choices;
}

catalog::LaunchChoices systemChoices(const LaunchLibrary& library, const catalog::SystemInfo& system,
                                     const std::string& alternativeEmulator) {
    const auto assigned = library.assignedChoices.find(system.name.str());
    return launchChoices(library, system.name, std::nullopt, {},
                         assigned != library.assignedChoices.end()
                             ? std::optional<launcher::LaunchChoice>(assigned->second)
                             : std::nullopt,
                         alternativeEmulator);
}

std::string_view systemChoiceOrigin(const LaunchLibrary& library, std::string_view system) {
    if (library.assignedChoices.find(system) != library.assignedChoices.end()) {
        return kAssignedOrigin;
    }
    return library.profiles.systemChoice(system) != nullptr ? kEmulatorsJsonOrigin : std::string_view();
}

void setProgramChoices(LaunchLibrary& library, const std::vector<catalog_db::ProgramChoiceRow>& rows) {
    library.programChoices.clear();
    for (const catalog_db::ProgramChoiceRow& row : rows) {
        library.programChoices.emplace(row.emulator, row.program);
    }
}

void loadAssignedChoices(LaunchLibrary& library, const core::Paths& paths) {
    catalog_db::DatabaseOptions options;
    options.create = false;
    options.switchToWal = false; // reading writes nothing into the file
    const auto unknown = [&](const Error& error) {
        library.issues.push_back(catalog::Issue{
            catalog::IssueSeverity::Warning, "catalog-unreadable", {},
            core::pathToUtf8Lossy(catalog_db::databaseFile(paths)),
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
    auto programs = (*store)->programChoices(library.profile.id);
    if (!programs) {
        unknown(programs.error());
        return;
    }
    setProgramChoices(library, *programs);
}

catalog::LaunchResolution resolveCatalogLaunch(const LaunchLibrary& library, const catalog_db::GameDetails& details,
                                               const std::optional<launcher::LaunchChoice>& gameChoice,
                                               const std::optional<launcher::LaunchChoice>& assignedSystem,
                                               GameFileCheck check) {
    const catalog::CatalogEntry& entry = details.indexed.entry;
    const catalog::SystemInfo* system = library.findSystem(entry.system.str());
    if (system == nullptr) {
        return notConfigured(entry.system, gameChoice);
    }
    catalog::LaunchGame game;
    game.relativePath = entry.key.relativePath();
    game.directory = entry.file && entry.file->directory;
    game.title = entry.title;
    if (system->romFolder.empty()) {
        game.availability = catalog::Availability::SourceUnavailable;
        game.unavailableReason = catalog::SourceUnavailableReason::FolderMissing;
    } else if (check == GameFileCheck::Disk) {
        findOnDisk(*system, game);
    } else {
        game.availability = entry.availability;
        game.unavailableReason = entry.unavailableReason;
    }
    const std::string alternativeEmulator = details.system ? details.system->alternativeEmulator : std::string();
    const catalog::LaunchChoices choices =
        launchChoices(library, entry.system, gameChoice, entry.altEmulator, assignedSystem, alternativeEmulator);
    return catalog::resolveLaunch(*system, &game, choices, library.context());
}

catalog::LaunchResolution resolveSystemLaunch(const LaunchLibrary& library, const catalog::SystemInfo& catalogSystem,
                                              const std::optional<launcher::LaunchChoice>& assignedSystem) {
    const catalog::SystemInfo* system = library.findSystem(catalogSystem.name.str());
    if (system == nullptr) {
        return notConfigured(catalogSystem.name, std::nullopt);
    }
    const catalog::LaunchChoices choices = launchChoices(library, catalogSystem.name, std::nullopt, {}, assignedSystem,
                                                         catalogSystem.alternativeEmulator);
    return catalog::resolveLaunch(*system, nullptr, choices, library.context());
}

bool resolvedPerGame(const catalog::LaunchResolution& systemResolution) noexcept {
    return !systemResolution.translation.notes.empty();
}

} // namespace rv::library
