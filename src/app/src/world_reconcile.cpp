#include <rv/app/world_reconcile.hpp>

#include <cmath>
#include <map>

#include <glm/glm.hpp>

#include <rv/app/world_live_plan.hpp>
#include <rv/world/floor_layout.hpp>

namespace rv::app {
namespace {

/// The items of `before` and `after` by game, `samePlace` telling whether an item in both stands where it stood.
template <typename SamePlace>
[[nodiscard]] LayoutDifference compareBy(const world::CollectionLayout& before, const world::CollectionLayout& after,
                                         SamePlace&& samePlace) {
    std::map<catalog::GameId, const world::CollectionItem*> earlier;
    for (const world::CollectionItem& item : before.items) {
        earlier.emplace(item.game, &item);
    }
    LayoutDifference difference;
    for (const world::CollectionItem& item : after.items) {
        const auto found = earlier.find(item.game);
        if (found == earlier.end()) {
            ++difference.added;
            continue;
        }
        const world::CollectionItem& was = *found->second;
        earlier.erase(found);
        if (was.pickId != item.pickId || !samePlace(was.transform, item.transform)) {
            ++difference.moved;
        } else if (was.flags != item.flags) {
            ++difference.flagsChanged;
        } else {
            ++difference.kept;
        }
    }
    difference.removed = earlier.size();
    return difference;
}

/// A slot's place is whole millimetres and a rotation of whole hundredths of a degree: what tells two places apart is
/// far above these.
constexpr float kSameTranslation = 1e-4f; // meters
constexpr float kSameRotation = 1e-4f;

[[nodiscard]] bool nearlySame(const glm::mat4& a, const glm::mat4& b) noexcept {
    for (int column = 0; column < 4; ++column) {
        const float tolerance = column == 3 ? kSameTranslation : kSameRotation;
        for (int row = 0; row < 4; ++row) {
            if (!(std::abs(a[column][row] - b[column][row]) <= tolerance)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

LayoutDifference compareLayouts(const world::CollectionLayout& before, const world::CollectionLayout& after) {
    return compareBy(before, after, [](const glm::mat4& was, const glm::mat4& now) { return was == now; });
}

LayoutDifference compareLayouts(const world::CollectionLayout& before, const world::RoomPlacement& beforeAt,
                                const world::CollectionLayout& after, const world::RoomPlacement& afterAt) {
    if (beforeAt == afterAt) {
        return compareLayouts(before, after);
    }
    // Back into the room's frame from each placement (a half turn and a translation: its inverse is exact enough).
    const glm::mat4 fromBefore = glm::inverse(world::roomToFloorMatrix(beforeAt));
    const glm::mat4 fromAfter = glm::inverse(world::roomToFloorMatrix(afterAt));
    return compareBy(before, after, [&](const glm::mat4& was, const glm::mat4& now) {
        return nearlySame(fromBefore * was, fromAfter * now);
    });
}

std::string_view toString(Reconciliation reconciliation) noexcept {
    switch (reconciliation) {
    case Reconciliation::Textures:
        return "textures";
    case Reconciliation::Rebuild:
        return "rebuild";
    case Reconciliation::Shell:
        return "shell";
    }
    return "rebuild";
}

ReconcilePlan planReconciliation(const WorldBuild& shown, const WorldBuild& build) {
    ReconcilePlan plan;
    if (!build.plan || !build.scene) {
        plan.kind = Reconciliation::Shell;
        return plan;
    }
    const std::optional<ShownFloor> before = shownFloorOf(shown);
    if (!before) {
        plan.kind = Reconciliation::Rebuild;
        plan.difference.added = build.itemCount();
        return plan;
    }
    // The live update's decision, room by room (I4 step 6b): the stored floor in place unless its static part changed.
    plan.live = planLiveUpdate(*before, build);
    plan.difference = plan.live.difference;
    switch (plan.live.kind) {
    case LiveUpdateKind::Unchanged:
    case LiveUpdateKind::Textures:
        plan.kind = Reconciliation::Textures;
        plan.reloadCovers = plan.live.reloadCovers;
        break;
    case LiveUpdateKind::Items:
    case LiveUpdateKind::Rebuild:
        plan.kind = Reconciliation::Rebuild;
        break;
    case LiveUpdateKind::Kept:
        plan.kind = Reconciliation::Shell;
        break;
    }
    return plan;
}

} // namespace rv::app
