#include <rv/app/world_live_plan.hpp>

#include <algorithm>
#include <map>
#include <set>

#include <fmt/format.h>

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

/// Every room's covers, room after room, in their order (the floor's cover requests as one list).
template <typename Rooms>
[[nodiscard]] std::vector<CoverItem> allCovers(const Rooms& rooms) {
    std::vector<CoverItem> covers;
    for (const auto& room : rooms) {
        covers.insert(covers.end(), room.covers.begin(), room.covers.end());
    }
    return covers;
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
        shown.rooms.push_back(ShownCollection{room.room, room.layout, room.covers, room.scenePlacement});
    }
    shown.mediaKnown = build.mediaRoot.has_value();
    shown.screenGames = build.screenGames;
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

LiveUpdatePlan planLiveUpdate(const ShownFloor& shown, const WorldBuild& build) {
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

    if (plan.difference.moved > 0) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = fmt::format("{} item(s) of the floor shown stand elsewhere in the build", plan.difference.moved);
        return plan;
    }
    if (build.floor != shown.floor) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = "another floor";
        return plan;
    }
    if (build.planDigest == shown.planDigest && plan.difference.same() && build.sceneDigest == shown.sceneDigest) {
        // The floor as it shows; at most its pictures.
        const bool coversDiffer = allCovers(build.rooms) != allCovers(shown.rooms);
        plan.reloadCovers = coversDiffer || rootNowKnown;
        plan.kind = plan.reloadCovers ? LiveUpdateKind::Textures : LiveUpdateKind::Unchanged;
        return plan;
    }
    if (shown.sceneDigest.empty() || build.sceneDigest != shown.sceneDigest) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = "the floor's static part changed (a room's fixtures, dressing or size, a room added)";
        return plan;
    }
    if (build.rooms.size() != shown.rooms.size()) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = "the floor's collections changed";
        return plan;
    }
    for (const RoomCollection& room : build.rooms) {
        const ShownCollection* standing = shown.find(room.room);
        if (standing == nullptr || !sameFamilies(standing->layout, room.layout)) {
            plan.kind = LiveUpdateKind::Rebuild;
            plan.reason = "a collection's media family changed";
            return plan;
        }
    }
    plan.kind = LiveUpdateKind::Items;
    plan.reloadCovers = true;
    return plan;
}

} // namespace rv::app
