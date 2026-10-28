#include <rv/app/world_live_plan.hpp>

#include <algorithm>
#include <map>
#include <set>

#include <fmt/format.h>
#include <glm/glm.hpp>

#include <rv/world/floor_layout.hpp>

namespace rv::app {
namespace {

/// The cover requests of every room of `rooms` by game.
template <typename Rooms>
[[nodiscard]] std::map<catalog::GameId, const CoverItem*> coversByGame(const Rooms& rooms) {
    std::map<catalog::GameId, const CoverItem*> byGame;
    for (const auto& room : rooms) {
        for (const CoverItem& cover : room.covers) {
            byGame.emplace(cover.game, &cover);
        }
    }
    return byGame;
}

/// The groups draw with the same family's meshes (CollectionInstance keeps its meshes and materials).
[[nodiscard]] bool sameFamilies(const world::CollectionLayout& a, const world::CollectionLayout& b) {
    if (a.groups.size() != b.groups.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.groups.size(); ++index) {
        if (a.groups[index].mediaFamily != b.groups[index].mediaFamily ||
            a.groups[index].item != b.groups[index].item || a.groups[index].itemSize != b.groups[index].itemSize) {
            return false;
        }
    }
    return true;
}

} // namespace

const ShownCollection* ShownFloor::find(const world::RoomId& room) const noexcept {
    for (const ShownCollection& collection : rooms) {
        if (collection.room == room) {
            return &collection;
        }
    }
    return nullptr;
}

std::optional<ShownFloor> shownFloorOf(const WorldBuild& build) {
    // A build shown keeps its plan and its collections (its scene is given to the application when it shows).
    if (!build.plan) {
        return std::nullopt;
    }
    ShownFloor shown;
    shown.planDigest = build.planDigest;
    shown.floor = build.floor;
    shown.sceneDigest = build.sceneDigest;
    for (const RoomCollection& room : build.rooms) {
        shown.rooms.push_back(ShownCollection{room.room, room.layout, room.covers, room.scenePlacement,
                                              room.contentDigest, room.screenGames});
    }
    shown.mediaKnown = build.mediaRoot.has_value();
    shown.screenGames = build.screenGames;
    shown.floorPlacement = build.floorPlacement;
    return shown;
}

LayoutDifference compareFloors(const ShownFloor& shown, const WorldBuild& build) {
    LayoutDifference difference;
    std::set<world::RoomId> seen;
    for (const RoomCollection& room : build.rooms) {
        seen.insert(room.room);
        const ShownCollection* before = shown.find(room.room);
        if (before == nullptr) {
            difference.added += room.layout.items.size();
            continue;
        }
        const LayoutDifference part =
            compareLayouts(before->layout, before->scenePlacement, room.layout, room.scenePlacement);
        difference.kept += part.kept;
        difference.moved += part.moved;
        difference.flagsChanged += part.flagsChanged;
        difference.added += part.added;
        difference.removed += part.removed;
    }
    for (const ShownCollection& room : shown.rooms) {
        if (!seen.contains(room.room)) {
            difference.removed += room.layout.items.size();
        }
    }
    return difference;
}

const RoomUpdate* LiveUpdatePlan::find(const world::RoomId& room) const noexcept {
    for (const RoomUpdate& update : rooms) {
        if (update.room == room) {
            return &update;
        }
    }
    return nullptr;
}

std::string_view toString(RoomUpdateKind kind) noexcept {
    switch (kind) {
    case RoomUpdateKind::Unchanged:
        return "unchanged";
    case RoomUpdateKind::Textures:
        return "textures";
    case RoomUpdateKind::Items:
        return "items";
    case RoomUpdateKind::Added:
        return "added";
    case RoomUpdateKind::Removed:
        return "removed";
    }
    return "unchanged";
}

std::optional<FrameMove> walkerMove(const ShownFloor& shown, const WorldBuild& build, std::string_view area) {
    std::optional<world::RoomPlacement> before;
    std::optional<world::RoomPlacement> after;
    for (const ShownCollection& room : shown.rooms) {
        if (room.room.toString() == area) {
            before = room.scenePlacement;
        }
    }
    for (const RoomCollection& room : build.rooms) {
        if (room.room.toString() == area) {
            after = room.scenePlacement;
        }
    }
    if (!before && !after) {
        // A hall (or an area of no collection room): it moves with the floor's frame.
        before = shown.floorPlacement;
        after = build.floorPlacement;
    }
    if (!before || !after || *before == *after) {
        return std::nullopt;
    }
    FrameMove move;
    move.matrix = world::roomToFloorMatrix(*after) * glm::inverse(world::roomToFloorMatrix(*before));
    move.halfTurn = (before->yawHalfTurns + after->yawHalfTurns) % 2 != 0;
    return move;
}

std::string_view toString(LiveUpdateKind kind) noexcept {
    switch (kind) {
    case LiveUpdateKind::Unchanged:
        return "unchanged";
    case LiveUpdateKind::Textures:
        return "textures";
    case LiveUpdateKind::Items:
        return "items";
    case LiveUpdateKind::Rebuild:
        return "rebuild";
    case LiveUpdateKind::Kept:
        return "kept";
    }
    return "rebuild";
}

LiveUpdatePlan planLiveUpdate(const ShownFloor& shown, const WorldBuild& build, bool movesInPlace) {
    LiveUpdatePlan plan;
    if (!build.plan || !build.scene) {
        plan.kind = LiveUpdateKind::Kept;
        plan.reason = build.reason.empty() ? std::string("the building's job has nothing to show") : build.reason;
        return plan;
    }
    plan.difference = compareFloors(shown, build);

    std::map<catalog::GameId, bool> before;
    for (const ShownCollection& room : shown.rooms) {
        for (const world::CollectionItem& item : room.layout.items) {
            before.emplace(item.game, false);
        }
    }
    for (const RoomCollection& room : build.rooms) {
        for (const world::CollectionItem& item : room.layout.items) {
            if (auto found = before.find(item.game); found != before.end()) {
                found->second = true;
            } else {
                plan.added.push_back(item.game);
            }
        }
    }
    for (const auto& [game, standing] : before) {
        if (!standing) {
            plan.removed.push_back(game);
        }
    }
    std::sort(plan.added.begin(), plan.added.end());

    const auto earlier = coversByGame(shown.rooms);
    for (const RoomCollection& room : build.rooms) {
        for (const CoverItem& cover : room.covers) {
            const auto found = earlier.find(cover.game);
            if (found != earlier.end() && !(*found->second == cover)) {
                plan.coversChanged.push_back(cover.game);
            }
        }
    }
    std::sort(plan.coversChanged.begin(), plan.coversChanged.end());
    plan.screenGamesChanged = build.screenGames != shown.screenGames;
    const bool rootNowKnown = build.mediaRoot.has_value() && !shown.mediaKnown;

    // Room by room (I4 step 6b): first by its content digest, then by its items and covers.
    bool collectionsChanged = false;
    bool familyChanged = false;
    std::set<world::RoomId> seen;
    for (const RoomCollection& room : build.rooms) {
        seen.insert(room.room);
        RoomUpdate update;
        update.room = room.room;
        const ShownCollection* standing = shown.find(room.room);
        if (standing == nullptr) {
            update.kind = RoomUpdateKind::Added;
            plan.rooms.push_back(std::move(update));
            continue;
        }
        update.difference =
            compareLayouts(standing->layout, standing->scenePlacement, room.layout, room.scenePlacement);
        for (const CoverItem& cover : room.covers) {
            const auto found = earlier.find(cover.game);
            update.coversChanged += found != earlier.end() && !(*found->second == cover) ? 1 : 0;
        }
        update.screenGamesChanged = standing->screenGames != room.screenGames;
        plan.screenGamesChanged = plan.screenGamesChanged || update.screenGamesChanged;
        // Its content digest tells whether the plan changed anything of it; its items (their availability is the
        // catalog's, not the plan's) and covers what shows of it.
        update.planChanged = room.contentDigest.empty() || room.contentDigest != standing->contentDigest ||
                             room.scenePlacement != standing->scenePlacement;
        const bool coversDiffer = room.covers != standing->covers;
        if (update.difference.same()) {
            update.kind = coversDiffer || rootNowKnown ? RoomUpdateKind::Textures : RoomUpdateKind::Unchanged;
        } else {
            update.kind = RoomUpdateKind::Items;
        }
        collectionsChanged = collectionsChanged || standing->layout.items.empty() != room.layout.items.empty();
        familyChanged = familyChanged || !sameFamilies(standing->layout, room.layout);
        plan.rooms.push_back(std::move(update));
    }
    for (const ShownCollection& room : shown.rooms) {
        if (!seen.contains(room.room)) {
            RoomUpdate update;
            update.room = room.room;
            update.kind = RoomUpdateKind::Removed;
            plan.rooms.push_back(std::move(update));
        }
    }

    if (plan.difference.moved > 0 && !movesInPlace) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = fmt::format("{} item(s) of the floor shown stand elsewhere in the build", plan.difference.moved);
        return plan;
    }
    if (build.floor != shown.floor) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = "another floor";
        return plan;
    }
    if (shown.sceneDigest.empty() || build.sceneDigest != shown.sceneDigest) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = "the floor's static part changed (a room's fixtures, dressing or size, a room added)";
        return plan;
    }
    const bool roomsChanged = std::any_of(plan.rooms.begin(), plan.rooms.end(), [](const RoomUpdate& update) {
        return update.kind == RoomUpdateKind::Added || update.kind == RoomUpdateKind::Removed;
    });
    if (roomsChanged || collectionsChanged) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = "the floor's collections changed";
        return plan;
    }
    if (familyChanged) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = "a collection's media family changed";
        return plan;
    }
    const auto any = [&](RoomUpdateKind kind) {
        return std::any_of(plan.rooms.begin(), plan.rooms.end(),
                           [kind](const RoomUpdate& update) { return update.kind == kind; });
    };
    if (any(RoomUpdateKind::Items)) {
        plan.kind = LiveUpdateKind::Items;
        plan.reloadCovers = true;
        return plan;
    }
    // The floor as it shows; at most its pictures.
    plan.reloadCovers = any(RoomUpdateKind::Textures);
    plan.kind = plan.reloadCovers ? LiveUpdateKind::Textures : LiveUpdateKind::Unchanged;
    return plan;
}

} // namespace rv::app
