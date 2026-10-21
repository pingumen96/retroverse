#pragma once

// Private to rv_cli_core (I3 steps 3 and 9, I4 step 1): what `worldplan generate` and `worldplan update` share: the
// rooms' selections of a building from canonical selection files or from the catalog, as the app's catalog worker
// makes them (I3 step 2: SqliteCatalogQuery::roomEntries on a read-only connection, esde::checkRoomMedia against the
// media root of the library <user data>/config/library.json records, world::selectRoom; nothing while the media are
// unknown) with the systems' platform tags, the building's data, the aliases the catalog records for placed games its
// snapshots no longer hold (ADR-0008 §4, for the update), the world seed option, the world configuration, and the
// change report as text and JSON. Header only, so that each subcommand keeps one source.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/room_entry.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/cli/command.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/room_input.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_plan.hpp>

#include "catalog_text.hpp"
#include "esde_text.hpp"
#include "library_snapshot.hpp"
#include "world_plan_text.hpp"

namespace rv::cli {

/// A world seed option: a decimal integer from 0 to 2^64 - 1 without leading zeros.
inline std::optional<std::uint64_t> parseWorldSeed(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || text.empty() ||
        (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    return value;
}

/// The world configuration (the preset under world.json, its issues logged), with `seed` over it when given. On
/// failure the message is on stderr and the command exits 3.
inline std::optional<world::WorldConfig> loadWorldConfiguration(CommandContext& context, std::string_view name,
                                                                std::optional<std::uint64_t> seed) {
    auto loaded = world::loadWorldConfig(context.paths.assetRoot(), context.paths.configDir());
    if (!loaded) {
        context.err.writeLine(fmt::format("{}: the world configuration: {}", name, loaded.error().toString()));
        return std::nullopt;
    }
    for (const Error& issue : loaded->issues) {
        spdlog::warn("world.json left out: {}", issue.toString());
    }
    world::WorldConfig config = std::move(loaded->config);
    if (seed) {
        config.seed = seed;
    }
    return config;
}

/// The building's data (building.json, systems.json) under the asset root. On failure the message is on stderr and the
/// command exits 3.
struct BuildingInfo {
    world::BuildingData building;
    world::SystemsData systems;
};
inline std::optional<BuildingInfo> loadBuildingInfo(CommandContext& context, std::string_view name) {
    auto building = world::loadBuildingData(context.paths.assetRoot());
    if (!building) {
        context.err.writeLine(fmt::format("{}: the building's data: {}", name, building.error().toString()));
        return std::nullopt;
    }
    auto systems = world::loadSystemsData(context.paths.assetRoot());
    if (!systems) {
        context.err.writeLine(fmt::format("{}: the systems' data: {}", name, systems.error().toString()));
        return std::nullopt;
    }
    return BuildingInfo{std::move(*building), std::move(*systems)};
}

/// The rooms' selections of a building and the library they are for, or how the command ends.
struct Selected {
    /// Set when the selections were made (possibly none).
    bool ok = false;
    std::vector<world::RoomSelection> selections;
    /// The systems' ES-DE platform tags (the catalog's; none for selection files).
    std::map<catalog::SystemName, std::vector<std::string>> platformTags;
    catalog::LibraryId library;
    /// The catalog's selections only: the snapshots' GameIds, for the aliases of placed games they lack.
    std::set<catalog::GameId> snapshotGames;
    bool fromCatalog = false;
    /// The catalog's systems left out of the building, and why (excluded, no placeable title, no data yet).
    std::vector<std::pair<catalog::SystemName, std::string>> leftOut;
    ExitCode failure = ExitCode::RuntimeFailure;
};

/// The selections of canonical selection files (each names its room's system), for the library `given` (--library)
/// names or library.json records.
inline Selected selectionsFromFiles(CommandContext& context, std::string_view name,
                                    const std::vector<std::string>& files, const std::optional<std::string>& given) {
    Selected selected;
    const auto library = libraryOf(context, name, given, selected.failure);
    if (!library) {
        return selected;
    }
    selected.library = *library;
    for (const std::string& file : files) {
        const std::filesystem::path path = core::pathFromUtf8(file);
        auto text = core::readTextFile(path);
        if (!text) {
            context.err.writeLine(fmt::format("{}: the selection: {}", name, text.error().toString()));
            return selected;
        }
        auto selection = world::readRoomSelection(*text, pathText(path));
        if (!selection) {
            context.err.writeLine(fmt::format("{}: the selection: {}", name, selection.error().toString()));
            return selected;
        }
        selected.selections.push_back(std::move(*selection));
    }
    selected.ok = true;
    return selected;
}

/// The catalog's selections of `systems` (each must be in the catalog: exit 1 "the catalog has no system"), and, with
/// none named or with `everyOther`, of every other system of the catalog that classify() gives a room and that has a
/// placeable title (ADR-0013 §3); those others that do not are left out, with why. Unknown media decide nothing
/// (ADR-0010 §3): no selection, exit 1, "<name>: <nothing> while the media are unknown: ...".
inline Selected selectionsFromCatalog(CommandContext& context, std::string_view name,
                                      const std::vector<catalog::SystemName>& systems,
                                      const std::optional<std::string>& given, const world::WorldConfig& config,
                                      std::string_view nothing, const BuildingInfo& info, bool everyOther = false) {
    Selected selected;
    selected.fromCatalog = true;
    const auto profile = readRecordedLibrary(context, name, selected.failure);
    if (!profile) {
        return selected;
    }
    if (given && *given != profile->id.str()) {
        context.err.writeLine(fmt::format("{}: --library {} is not the catalog's library {} (library.json); a "
                                          "selection of another library comes with --selection",
                                          name, *given, profile->id.str()));
        selected.failure = ExitCode::UsageError;
        return selected;
    }
    selected.library = profile->id;
    auto reader = openCatalogReader(context.paths);
    if (!reader) {
        selected.failure = catalogFailure(context, reader.error());
        return selected;
    }
    auto summaries = reader->query->systems();
    if (!summaries) {
        selected.failure = catalogFailure(context, summaries.error());
        return selected;
    }
    std::map<catalog::SystemName, std::vector<std::string>> tags;
    for (const catalog_db::SystemSummary& summary : *summaries) {
        if (summary.library == profile->id) {
            tags[summary.system.name] = summary.system.platforms;
        }
    }
    const bool whole = systems.empty() || everyOther;
    const std::set<catalog::SystemName> named(systems.begin(), systems.end());
    std::vector<catalog::SystemName> wanted = systems;
    if (whole) {
        for (const auto& [system, platforms] : tags) {
            if (named.contains(system)) {
                continue;
            }
            const world::Classification classification =
                world::classify(system, platforms, info.systems, info.building);
            if (!classification.hasRoom()) {
                selected.leftOut.emplace_back(system,
                                              fmt::format("no room ({})", world::toString(classification.kind())));
                continue;
            }
            wanted.push_back(system);
        }
    }
    esde::EsdeMediaResolver resolver(mediaRootOf(*profile));
    for (const catalog::SystemName& system : wanted) {
        // A system named is required; any other is left out when it cannot be planned.
        const bool other = !named.contains(system);
        auto snapshot = reader->query->roomEntries(system);
        if (!snapshot) {
            if (snapshot.error().code == ErrorCode::NotFound) {
                if (other) {
                    selected.leftOut.emplace_back(system, "no entries in the catalog");
                    continue;
                }
                context.err.writeLine(fmt::format("{}: the catalog has no system {}", name, system.str()));
                selected.failure = ExitCode::ProblemsFound;
                return selected;
            }
            selected.failure = catalogFailure(context, snapshot.error());
            return selected;
        }
        esde::checkRoomMedia(*snapshot, resolver);
        for (const catalog::RoomEntry& entry : snapshot->entries) {
            selected.snapshotGames.insert(entry.id);
        }
        auto selection = world::selectRoom(world::RoomInput{profile->id, std::move(*snapshot)}, config);
        if (!selection) {
            if (selection.error().code == ErrorCode::Unavailable) {
                context.err.writeLine(
                    fmt::format("{}: {} while the media are unknown: {}", name, nothing, selection.error().message));
                selected.failure = ExitCode::ProblemsFound;
                return selected;
            }
            context.err.writeLine(fmt::format("{}: the room's selection of {}: {}", name, system.str(),
                                              selection.error().toString()));
            return selected;
        }
        if (other && selection->titles.empty()) {
            selected.leftOut.emplace_back(system, "no placeable title");
            continue;
        }
        if (const auto found = tags.find(system); found != tags.end()) {
            selected.platformTags[system] = found->second;
        }
        selected.selections.push_back(std::move(*selection));
    }
    selected.ok = true;
    return selected;
}

/// The data of each selection's room (loadWorldData() with the system's platform tags), for a fresh building. A system
/// named explicitly (`strict`) whose data the asset root cannot give fails the command (exit 3, "the world's data");
/// otherwise it is left out of the selections, reported in `selected.leftOut`.
inline std::optional<std::vector<world::WorldData>> loadSelectionsData(CommandContext& context, std::string_view name,
                                                                       Selected& selected, bool strict) {
    std::vector<world::WorldData> rooms;
    std::vector<world::RoomSelection> kept;
    for (world::RoomSelection& selection : selected.selections) {
        world::RoomDataRequest request;
        request.system = selection.room.system();
        if (const auto tags = selected.platformTags.find(request.system); tags != selected.platformTags.end()) {
            request.platformTags = tags->second;
        }
        auto data = world::loadWorldData(context.paths.assetRoot(), request);
        if (!data) {
            if (strict) {
                context.err.writeLine(fmt::format("{}: the world's data of {}: {}", name, request.system.str(),
                                                  data.error().toString()));
                return std::nullopt;
            }
            selected.leftOut.emplace_back(request.system, "no data yet: " + data.error().message);
            continue;
        }
        rooms.push_back(std::move(*data));
        kept.push_back(std::move(selection));
    }
    selected.selections = std::move(kept);
    return rooms;
}

/// The aliases the catalog records for the placed games of `plan` that its snapshot (`snapshotGames`) lacks: each
/// such GameId that became an alias of another identity (ICatalogStore::alias). Errors: those of the catalog.
inline Expected<std::vector<world::GameAlias>> catalogAliases(const core::Paths& paths, const world::WorldPlan& plan,
                                                              const std::set<catalog::GameId>& snapshotGames) {
    std::vector<world::GameAlias> aliases;
    std::vector<catalog::GameId> missing;
    for (const world::Placement& placement : plan.placements) {
        if (!snapshotGames.contains(placement.game)) {
            missing.push_back(placement.game);
        }
    }
    if (missing.empty()) {
        return aliases;
    }
    auto reader = openCatalogReader(paths);
    if (!reader) {
        return propagate(reader.error());
    }
    auto store = catalog_db::SqliteCatalogStore::open(*reader->database);
    if (!store) {
        return propagate(store.error());
    }
    for (const catalog::GameId& game : missing) {
        auto alias = (*store)->alias(game);
        if (!alias) {
            return propagate(alias.error());
        }
        if (*alias) {
            aliases.push_back({game, (*alias)->game});
        }
    }
    return aliases;
}

/// The change report as text (the --diff listing of `worldplan update`, and the summary lines of any change).
inline std::string changeReportText(const world::ChangeReport& changes, bool detailed) {
    std::string text;
    const auto line = [&](std::string_view label, std::string_view value) {
        text += fmt::format("  {:<17}{}\n", fmt::format("{}:", label), value);
    };
    line("Outcome", fmt::format("{}, rebuild {}", world::toString(changes.outcome), world::toString(changes.rebuild)));
    if (!changes.regenerationReason.empty()) {
        line("Reason", changes.regenerationReason);
    }
    line("Changes", fmt::format("{} added, {} kept, {} newly unplaced; fixtures {} added, {} removed, {} moved; {} "
                                "level{} undressed, {} slot{} freed",
                                changes.added.size(), changes.kept, changes.unplaced.size(),
                                changes.fixturesAdded.size(), changes.fixturesRemoved.size(),
                                changes.fixturesMoved.size(), changes.dressingRemoved.size(),
                                changes.dressingRemoved.size() == 1 ? "" : "s", changes.slotsFreed.size(),
                                changes.slotsFreed.size() == 1 ? "" : "s"));
    line("Pins", fmt::format("{} orphaned, {} need a confirmation", changes.orphanedPins.size(),
                             changes.needsConfirmation.size()));
    for (const world::KeptRoom& kept : changes.keptRooms) {
        line("Room kept", fmt::format("{}: {}", kept.room.toString(), kept.reason));
    }
    for (const world::KeptRoom& held : changes.heldRooms) {
        line("Room held", fmt::format("{}: {}", held.room.toString(), held.reason));
    }
    // The systems that entered the building (I4 step 6a): each in the room it opened or as a section of one.
    for (const world::SystemRoom& entered : changes.systemsEntered) {
        const bool opened = std::find(changes.roomsAdded.begin(), changes.roomsAdded.end(), entered.room) !=
                            changes.roomsAdded.end();
        line("Entered", fmt::format("{} in {}{}", entered.system.str(), entered.room.toString(),
                                    opened ? " (a new room)" : " (a new section)"));
    }
    for (const world::SystemRoom& waiting : changes.systemsWaiting) {
        line("Waiting", fmt::format("{} for {} (held: an update given every section's selection enters it)",
                                    waiting.system.str(), waiting.room.toString()));
    }
    line("Digest before", changes.digestBefore.empty() ? std::string("none") : changes.digestBefore);
    line("Digest after", changes.digestAfter);
    if (!detailed) {
        return text;
    }
    const auto list = [&](std::string_view heading, const auto& items, const auto& describe) {
        if (items.empty()) {
            return;
        }
        text += fmt::format("{}:\n", heading);
        for (const auto& item : items) {
            text += fmt::format("  {}\n", describe(item));
        }
    };
    const auto id = [](const auto& value) { return value.toString(); };
    const auto plain = [](const std::string& value) { return value; };
    list("Added", changes.added, id);
    list("Newly unplaced", changes.unplaced, id);
    list("Fixtures added", changes.fixturesAdded, id);
    list("Fixtures removed", changes.fixturesRemoved, id);
    list("Fixtures moved", changes.fixturesMoved, id);
    list("Dressing removed", changes.dressingRemoved, id);
    list("Slots freed", changes.slotsFreed, id);
    list("Rooms kept", changes.keptRooms, [](const world::KeptRoom& kept) {
        return fmt::format("{}: {}", kept.room.toString(), kept.reason);
    });
    list("Rooms added", changes.roomsAdded, id);
    list("Orphaned pins", changes.orphanedPins, plain);
    list("Needs confirmation", changes.needsConfirmation, plain);
    return text;
}

/// The rooms an update kept, as JSON: [{"room", "reason"}].
inline CatalogJson keptRoomsJson(const std::vector<world::KeptRoom>& rooms) {
    CatalogJson array = CatalogJson::array();
    for (const world::KeptRoom& kept : rooms) {
        array.push_back(CatalogJson{{"room", kept.room.toString()}, {"reason", kept.reason}});
    }
    return array;
}

/// Systems with their rooms, as JSON: [{"system", "room"}].
inline CatalogJson systemRoomsJson(const std::vector<world::SystemRoom>& systems) {
    CatalogJson array = CatalogJson::array();
    for (const world::SystemRoom& entry : systems) {
        array.push_back(CatalogJson{{"system", entry.system.str()}, {"room", entry.room.toString()}});
    }
    return array;
}

/// The change report as JSON: outcome, rebuild, reason, the lists (ids as text), kept, the digests.
inline CatalogJson changeReportJson(const world::ChangeReport& changes) {
    const auto ids = [](const auto& items) {
        CatalogJson array = CatalogJson::array();
        for (const auto& item : items) {
            array.push_back(item.toString());
        }
        return array;
    };
    return CatalogJson{{"outcome", world::toString(changes.outcome)},
                       {"rebuild", world::toString(changes.rebuild)},
                       {"regenerationReason", changes.regenerationReason},
                       {"added", ids(changes.added)},
                       {"kept", changes.kept},
                       {"unplaced", ids(changes.unplaced)},
                       {"fixturesAdded", ids(changes.fixturesAdded)},
                       {"fixturesRemoved", ids(changes.fixturesRemoved)},
                       {"fixturesMoved", ids(changes.fixturesMoved)},
                       {"dressingRemoved", ids(changes.dressingRemoved)},
                       {"slotsFreed", ids(changes.slotsFreed)},
                       {"orphanedPins", changes.orphanedPins},
                       {"needsConfirmation", changes.needsConfirmation},
                       {"keptRooms", keptRoomsJson(changes.keptRooms)},
                       {"heldRooms", keptRoomsJson(changes.heldRooms)},
                       {"roomsAdded", ids(changes.roomsAdded)},
                       {"systemsEntered", systemRoomsJson(changes.systemsEntered)},
                       {"systemsWaiting", systemRoomsJson(changes.systemsWaiting)},
                       {"digestBefore", changes.digestBefore},
                       {"digestAfter", changes.digestAfter}};
}

/// Writes the hard findings of a validation on stderr and the soft ones to the log; the count of hard ones.
inline std::size_t reportFindings(CommandContext& context, std::string_view name,
                                  const world::ValidationReport& validation) {
    std::size_t hard = 0;
    for (const world::ValidationFinding& finding : validation.findings) {
        if (finding.severity == world::Severity::Hard) {
            ++hard;
            context.err.writeLine(fmt::format("{}: {} {}: {}", name, finding.rule, finding.subject, finding.message));
        } else {
            spdlog::warn("{}: soft finding {} {}: {}", name, finding.rule, finding.subject, finding.message);
        }
    }
    return hard;
}

} // namespace rv::cli
