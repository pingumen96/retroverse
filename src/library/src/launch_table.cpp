#include <rv/library/launch_table.hpp>

#include <algorithm>
#include <set>
#include <span>
#include <utility>

namespace rv::library {
namespace {

/// How many games the most recently played are looked for among: the directory's query, every system's.
constexpr std::size_t kRecentWindow = 2000;

} // namespace

SystemLaunchRow systemLaunchRow(const LaunchLibrary& library, const catalog::SystemInfo& system,
                                const std::string& alternativeEmulator) {
    SystemLaunchRow row;
    row.alternativeEmulator = alternativeEmulator;
    const catalog::SystemInfo* configured = library.findSystem(system.name.str());
    if (configured == nullptr) {
        row.system = system;
        row.resolution = resolveSystemLaunch(library, system, std::nullopt);
        return row;
    }
    row.system = *configured;
    const catalog::LaunchChoices choices = systemChoices(library, *configured, alternativeEmulator);
    row.systemChoice = choices.system;
    const catalog::LaunchContext context = library.context();
    row.resolution = catalog::resolveLaunch(*configured, nullptr, choices, context);
    if (const esde::SystemDefinition* definition = library.systems.find(system.name.str())) {
        for (const esde::SystemCommand& command : definition->commands) {
            row.commands.push_back(LaunchOption{
                launcher::LaunchChoice{launcher::LaunchChoiceKind::EsdeCommand, command.label}, command.text,
                library.translator->translate(configured->name, command.label, nullptr)});
        }
    }
    for (const launcher::EmulatorProfile* profile : library.profiles.serving(system.name.str())) {
        row.profiles.push_back(LaunchOption{launcher::LaunchChoice{launcher::LaunchChoiceKind::Profile, profile->id},
                                            profile->displayName,
                                            catalog::translateProfile(*configured, nullptr, *profile, context)});
    }
    return row;
}

std::vector<SystemLaunchRow> launchTable(LaunchLibrary& library, std::optional<std::string_view> only) {
    std::vector<SystemLaunchRow> rows;
    for (const catalog::SystemInfo& system : library.systemInfos) {
        if (only && system.name.str() != *only) {
            continue;
        }
        std::string alternativeEmulator;
        if (const auto gamelist = readSystemGamelist(library, system)) {
            alternativeEmulator = gamelist->alternativeEmulator;
        }
        rows.push_back(systemLaunchRow(library, system, alternativeEmulator));
    }
    return rows;
}

std::vector<LaunchOption> suggestedOptions(const SystemLaunchRow& row) {
    std::vector<LaunchOption> options = row.commands;
    options.insert(options.end(), row.profiles.begin(), row.profiles.end());
    std::stable_partition(options.begin(), options.end(), [](const LaunchOption& option) {
        return option.translation.readiness() == catalog::LaunchReadiness::Ready;
    });
    return options;
}

Expected<std::vector<SampleGame>> sampleGames(catalog_db::SqliteCatalogQuery& query, const catalog::SystemName& system,
                                              const catalog::LocalTimeToUtc& localToUtc, std::size_t limit) {
    catalog::CatalogQuery request;
    request.systems = {system};
    auto ids = query.run(request);
    if (!ids) {
        return propagate(ids.error());
    }
    catalog::RecentGamesQuery recentQuery;
    recentQuery.limit = kRecentWindow;
    recentQuery.localToUtc = localToUtc;
    auto recent = query.favoritesAndRecent(recentQuery);
    if (!recent) {
        return propagate(recent.error());
    }
    std::vector<catalog::RecentGame> played;
    for (catalog::RecentGame& game : *recent) {
        if (game.system == system && game.lastPlayed) {
            played.push_back(std::move(game));
        }
    }
    std::stable_sort(played.begin(), played.end(), [](const catalog::RecentGame& left, const catalog::RecentGame& right) {
        return *left.lastPlayed > *right.lastPlayed;
    });
    std::vector<SampleGame> games;
    std::set<std::uint64_t> taken;
    for (const catalog::RecentGame& game : played) {
        if (games.size() < limit && taken.insert(game.id.value()).second) {
            games.push_back(SampleGame{game.id, game.title});
        }
    }
    const std::size_t wanted = std::min(ids->size(), limit);
    std::vector<catalog::GameId> rest;
    for (const catalog::GameId id : *ids) {
        if (games.size() + rest.size() >= wanted) {
            break;
        }
        if (taken.insert(id.value()).second) {
            rest.push_back(id);
        }
    }
    auto rows = query.fetch(std::span<const catalog::GameId>(rest));
    if (!rows) {
        return propagate(rows.error());
    }
    for (const catalog::CatalogRow& row : *rows) {
        games.push_back(SampleGame{row.id, row.title});
    }
    return games;
}

} // namespace rv::library
