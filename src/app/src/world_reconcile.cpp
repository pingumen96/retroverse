#include <rv/app/world_reconcile.hpp>

#include <map>

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
    if (!build.plan || !build.scene || !build.sceneData || !build.layout) {
        plan.kind = Reconciliation::Shell;
        return plan;
    }
    if (shown.layout) {
        plan.difference = compareLayouts(*shown.layout, *build.layout);
    } else {
        plan.difference.added = build.layout->items.size();
    }
    if (!shown.plan || !shown.layout || build.planDigest != shown.planDigest || !plan.difference.same()) {
        plan.kind = Reconciliation::Rebuild;
        return plan;
    }
    plan.kind = Reconciliation::Textures;
    plan.reloadCovers = build.covers != shown.covers || build.mediaRoot.has_value();
    return plan;
}

} // namespace rv::app
