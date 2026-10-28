#pragma once

// Private to rv_cli_core (I4b step 5): what `worldplan regenerate` and `worldplan generate --replace` share: a
// regeneration's impact (ADR-0015 §5's preview) as text and as JSON, and the loader of the rooms' data that serves the
// data a command read already.

#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/cli/command.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/regeneration.hpp>

#include "catalog_text.hpp"

namespace rv::cli {

/// "1 regenerated, 0 added, ..." style counts of a list.
inline std::string countText(std::size_t count, std::string_view one, std::string_view many) {
    return fmt::format("{} {}", count, count == 1 ? one : many);
}

/// A conflict as one line: "<pin>: <reason>: <message>".
inline std::string conflictText(const world::OverrideConflict& conflict) {
    return fmt::format("{}: {}: {}", conflict.pin, world::toString(conflict.reason), conflict.message);
}

/// The impact's summary lines (each "  <label>: <value>"), and with `detailed` every item.
inline std::string impactText(const world::RegenerationImpact& impact, bool detailed) {
    std::string text;
    const auto line = [&](std::string_view label, std::string_view value) {
        text += fmt::format("  {:<17}{}\n", fmt::format("{}:", label), value);
    };
    line("Rooms", fmt::format("{} regenerated, {} added, {} removed, {} moved", impact.roomsRegenerated.size(),
                              impact.roomsAdded.size(), impact.roomsRemoved.size(), impact.roomsMoved.size()));
    line("Games", fmt::format("{} change slot, {} enter face-out, {} leave it, {} newly placed, {} newly unplaced",
                              impact.gamesMoved.size(), impact.faceOutEntering.size(), impact.faceOutLeaving.size(),
                              impact.newlyPlaced.size(), impact.newlyUnplaced.size()));
    line("Fixtures", fmt::format("{} added, {} removed, {} moved", impact.fixturesAdded.size(),
                                 impact.fixturesRemoved.size(), impact.fixturesMoved.size()));
    line("Pins", fmt::format("{} kept, {} conflicting, {} dropped", impact.pinsKept.size(), impact.conflicts.size(),
                             impact.overridesDropped.size()));
    for (const world::OverrideConflict& conflict : impact.conflicts) {
        line("Conflict", conflictText(conflict));
    }
    for (const std::string& pin : impact.overridesDropped) {
        line("Dropped", pin);
    }
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
    list("Rooms regenerated", impact.roomsRegenerated, id);
    list("Rooms added", impact.roomsAdded, id);
    list("Rooms removed", impact.roomsRemoved, id);
    list("Rooms moved", impact.roomsMoved, id);
    list("Games moved", impact.gamesMoved, [](const world::GameMove& move) {
        return fmt::format("{}: {} -> {}", move.game.toString(), move.from.toString(), move.to.toString());
    });
    list("Entering face-out", impact.faceOutEntering, id);
    list("Leaving face-out", impact.faceOutLeaving, id);
    list("Newly placed", impact.newlyPlaced, id);
    list("Newly unplaced", impact.newlyUnplaced, id);
    list("Fixtures added", impact.fixturesAdded, id);
    list("Fixtures removed", impact.fixturesRemoved, id);
    list("Fixtures moved", impact.fixturesMoved, id);
    list("Pins kept", impact.pinsKept, plain);
    return text;
}

/// The impact as JSON: every list (ids as text, the conflicts as {pin, game, fixture, reason, message}) and "counts".
inline CatalogJson impactJson(const world::RegenerationImpact& impact) {
    const auto ids = [](const auto& items) {
        CatalogJson array = CatalogJson::array();
        for (const auto& item : items) {
            array.push_back(item.toString());
        }
        return array;
    };
    CatalogJson moves = CatalogJson::array();
    for (const world::GameMove& move : impact.gamesMoved) {
        moves.push_back(CatalogJson{
            {"game", move.game.toString()}, {"from", move.from.toString()}, {"to", move.to.toString()}});
    }
    CatalogJson conflicts = CatalogJson::array();
    for (const world::OverrideConflict& conflict : impact.conflicts) {
        conflicts.push_back(CatalogJson{
            {"pin", conflict.pin},
            {"game", conflict.game.isValid() ? CatalogJson(conflict.game.toString()) : CatalogJson(nullptr)},
            {"fixture", conflict.fixture.empty() ? CatalogJson(nullptr) : CatalogJson(conflict.fixture.toString())},
            {"reason", std::string(world::toString(conflict.reason))},
            {"message", conflict.message}});
    }
    CatalogJson counts{{"roomsRegenerated", impact.roomsRegenerated.size()},
                       {"roomsAdded", impact.roomsAdded.size()},
                       {"roomsRemoved", impact.roomsRemoved.size()},
                       {"roomsMoved", impact.roomsMoved.size()},
                       {"gamesMoved", impact.gamesMoved.size()},
                       {"faceOutEntering", impact.faceOutEntering.size()},
                       {"faceOutLeaving", impact.faceOutLeaving.size()},
                       {"newlyPlaced", impact.newlyPlaced.size()},
                       {"newlyUnplaced", impact.newlyUnplaced.size()},
                       {"fixturesAdded", impact.fixturesAdded.size()},
                       {"fixturesRemoved", impact.fixturesRemoved.size()},
                       {"fixturesMoved", impact.fixturesMoved.size()},
                       {"pinsKept", impact.pinsKept.size()},
                       {"conflicts", impact.conflicts.size()},
                       {"overridesDropped", impact.overridesDropped.size()}};
    return CatalogJson{{"counts", std::move(counts)},
                       {"roomsRegenerated", ids(impact.roomsRegenerated)},
                       {"roomsAdded", ids(impact.roomsAdded)},
                       {"roomsRemoved", ids(impact.roomsRemoved)},
                       {"roomsMoved", ids(impact.roomsMoved)},
                       {"gamesMoved", std::move(moves)},
                       {"faceOutEntering", ids(impact.faceOutEntering)},
                       {"faceOutLeaving", ids(impact.faceOutLeaving)},
                       {"newlyPlaced", ids(impact.newlyPlaced)},
                       {"newlyUnplaced", ids(impact.newlyUnplaced)},
                       {"fixturesAdded", ids(impact.fixturesAdded)},
                       {"fixturesRemoved", ids(impact.fixturesRemoved)},
                       {"fixturesMoved", ids(impact.fixturesMoved)},
                       {"pinsKept", impact.pinsKept},
                       {"conflicts", std::move(conflicts)},
                       {"overridesDropped", impact.overridesDropped}};
}

/// A loader of the rooms' data (world::assetRootLoader() of the asset root) that first serves `systems`' data, read
/// already as a new room's (loadWorldData() of a system with its platform tags, no family, the default template).
inline world::RoomDataLoader preloadedLoader(const std::filesystem::path& assetRoot,
                                             std::span<const world::WorldData> systems,
                                             const std::map<catalog::SystemName, std::vector<std::string>>& tags) {
    auto read = std::make_shared<std::map<catalog::SystemName, world::WorldData>>();
    for (const world::WorldData& data : systems) {
        read->emplace(data.platform.system, data);
    }
    return [base = world::assetRootLoader(assetRoot), read,
            tags](const world::RoomDataRequest& request) -> Expected<world::WorldData> {
        const auto known = tags.find(request.system);
        const std::vector<std::string> tagsOf = known != tags.end() ? known->second : std::vector<std::string>{};
        const bool fresh = request.kind == world::RoomKind::Console && request.floor.empty() &&
                           request.mediaFamily.empty() && request.templateId == world::kDefaultRoomTemplate &&
                           request.platformTags == tagsOf;
        if (fresh) {
            if (const auto found = read->find(request.system); found != read->end()) {
                return found->second;
            }
        }
        return base(request);
    };
}

} // namespace rv::cli
