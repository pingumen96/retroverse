#include <rv/app/world_live_plan.hpp>

#include <algorithm>
#include <map>

#include <fmt/format.h>

namespace rv::app {
namespace {

/// The cover requests by game.
[[nodiscard]] std::map<catalog::GameId, const CoverItem*> coversByGame(const std::vector<CoverItem>& covers) {
    std::map<catalog::GameId, const CoverItem*> byGame;
    for (const CoverItem& cover : covers) {
        byGame.emplace(cover.game, &cover);
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

std::optional<ShownRoom> shownRoomOf(const WorldBuild& build) {
    if (!build.plan || !build.layout) {
        return std::nullopt;
    }
    ShownRoom shown;
    shown.planDigest = build.planDigest;
    shown.sceneDigest = build.sceneDigest;
    shown.layout = *build.layout;
    shown.covers = build.covers;
    shown.mediaKnown = build.mediaRoot.has_value();
    shown.screenGames = build.screenGames;
    return shown;
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

LiveUpdatePlan planLiveUpdate(const ShownRoom& shown, const WorldBuild& build) {
    LiveUpdatePlan plan;
    if (!build.plan || !build.scene || !build.sceneData || !build.layout) {
        plan.kind = LiveUpdateKind::Kept;
        plan.reason = build.reason.empty() ? std::string("the room's job has nothing to show") : build.reason;
        return plan;
    }
    const world::CollectionLayout& after = *build.layout;
    plan.difference = compareLayouts(shown.layout, after);

    std::map<catalog::GameId, bool> before;
    for (const world::CollectionItem& item : shown.layout.items) {
        before.emplace(item.game, false);
    }
    for (const world::CollectionItem& item : after.items) {
        if (auto found = before.find(item.game); found != before.end()) {
            found->second = true;
        } else {
            plan.added.push_back(item.game);
        }
    }
    for (const auto& [game, standing] : before) {
        if (!standing) {
            plan.removed.push_back(game);
        }
    }
    std::sort(plan.added.begin(), plan.added.end());

    const auto earlier = coversByGame(shown.covers);
    for (const CoverItem& cover : build.covers) {
        const auto found = earlier.find(cover.game);
        if (found != earlier.end() && !(*found->second == cover)) {
            plan.coversChanged.push_back(cover.game);
        }
    }
    std::sort(plan.coversChanged.begin(), plan.coversChanged.end());
    plan.screenGamesChanged = build.screenGames != shown.screenGames;
    const bool rootNowKnown = build.mediaRoot.has_value() && !shown.mediaKnown;

    if (plan.difference.moved > 0) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = fmt::format("{} item(s) of the room shown stand elsewhere in the build", plan.difference.moved);
        return plan;
    }
    if (build.planDigest == shown.planDigest && plan.difference.same() && build.sceneDigest == shown.sceneDigest) {
        // The room as it shows; at most its pictures.
        const bool coversDiffer = build.covers != shown.covers;
        plan.reloadCovers = coversDiffer || rootNowKnown;
        plan.kind = plan.reloadCovers ? LiveUpdateKind::Textures : LiveUpdateKind::Unchanged;
        return plan;
    }
    if (shown.sceneDigest.empty() || build.sceneDigest != shown.sceneDigest) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = "the room's static part changed (fixtures, dressing or the room's size)";
        return plan;
    }
    if (!sameFamilies(shown.layout, after)) {
        plan.kind = LiveUpdateKind::Rebuild;
        plan.reason = "the collection's media family changed";
        return plan;
    }
    plan.kind = LiveUpdateKind::Items;
    plan.reloadCovers = true;
    return plan;
}

} // namespace rv::app
