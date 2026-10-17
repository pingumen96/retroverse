#include <rv/app/catalog_launch_library.hpp>

#include <algorithm>
#include <utility>

#include <fmt/format.h>

#include <rv/core/paths.hpp>

namespace rv::app {
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

const catalog::SystemInfo* CatalogLaunchLibrary::findSystem(std::string_view name) const {
    const auto found = std::find_if(systemInfos.begin(), systemInfos.end(),
                                    [&](const catalog::SystemInfo& system) { return system.name.str() == name; });
    return found == systemInfos.end() ? nullptr : &*found;
}

Expected<std::unique_ptr<CatalogLaunchLibrary>> loadCatalogLaunchLibrary(catalog::LibraryProfile profile,
                                                                        const core::Paths& paths) {
    auto library = std::make_unique<CatalogLaunchLibrary>();
    library->profile = std::move(profile);
    auto settings = esde::readSettings(library->profile.esdeHome);
    if (!settings) {
        Error error = settings.error();
        error.message = "ES-DE's settings cannot be read: " + error.message;
        return propagate(std::move(error));
    }
    library->settings = std::move(*settings);
    library->romRoot = esde::resolveRomRoot(library->settings, library->profile);
    auto systems = esde::loadSystems(library->profile);
    if (!systems) {
        Error error = systems.error();
        error.message = "ES-DE's systems cannot be read: " + error.message;
        return propagate(std::move(error));
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
        esde::TranslationContext{library->profile, library->romRoot, &library->findRules, &library->probe},
        library->systems);
    return library;
}

catalog::LaunchChoices catalogLaunchChoices(const CatalogLaunchLibrary& library, const catalog::SystemName& system,
                                            const std::optional<launcher::LaunchChoice>& game,
                                            std::string gameAltEmulator,
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

catalog::LaunchResolution resolveCatalogLaunch(const CatalogLaunchLibrary& library,
                                               const catalog_db::GameDetails& details,
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
        catalogLaunchChoices(library, entry.system, gameChoice, entry.altEmulator, assignedSystem, alternativeEmulator);
    return catalog::resolveLaunch(*system, &game, choices, library.context());
}

catalog::LaunchResolution resolveSystemLaunch(const CatalogLaunchLibrary& library,
                                              const catalog::SystemInfo& catalogSystem,
                                              const std::optional<launcher::LaunchChoice>& assignedSystem) {
    const catalog::SystemInfo* system = library.findSystem(catalogSystem.name.str());
    if (system == nullptr) {
        return notConfigured(catalogSystem.name, std::nullopt);
    }
    const catalog::LaunchChoices choices = catalogLaunchChoices(library, catalogSystem.name, std::nullopt, {},
                                                                assignedSystem, catalogSystem.alternativeEmulator);
    return catalog::resolveLaunch(*system, nullptr, choices, library.context());
}

bool resolvedPerGame(const catalog::LaunchResolution& systemResolution) noexcept {
    return !systemResolution.translation.notes.empty();
}

} // namespace rv::app
