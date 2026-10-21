#include <rv/app/world_reconcile.hpp>

#include <map>

#include <rv/app/world_live_plan.hpp>

namespace rv::app {

LayoutDifference compareLayouts(const world::CollectionLayout& before, const world::CollectionLayout& after) {
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
        if (was.pickId != item.pickId || was.transform != item.transform) {
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
    if (before) {
        plan.difference = compareFloors(*before, build);
    } else {
        plan.difference.added = build.itemCount();
    }
    if (!before || build.planDigest != shown.planDigest || build.floor != shown.floor || !plan.difference.same()) {
        plan.kind = Reconciliation::Rebuild;
        return plan;
    }
    plan.kind = Reconciliation::Textures;
    bool coversDiffer = build.rooms.size() != shown.rooms.size();
    for (std::size_t index = 0; !coversDiffer && index < build.rooms.size(); ++index) {
        coversDiffer = build.rooms[index].room != shown.rooms[index].room ||
                       build.rooms[index].covers != shown.rooms[index].covers;
    }
    plan.reloadCovers = coversDiffer || build.mediaRoot.has_value();
    return plan;
}

} // namespace rv::app
