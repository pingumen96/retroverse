#include <rv/app/world_edit_room.hpp>

#include <utility>
#include <vector>

namespace rv::app {

Expected<std::unique_ptr<EditRoom>> EditRoom::build(std::shared_ptr<const world::CollectionLayout> layout) {
    std::unique_ptr<EditRoom> room(new EditRoom());
    room->m_layout = std::move(layout);
    const world::CollectionLayout& shown = *room->m_layout;
    std::vector<world::PickTarget> targets = shown.pickTargets();
    for (std::size_t index = 0; index < shown.items.size(); ++index) {
        room->m_items.emplace(shown.items[index].pickId, index);
        room->m_byGame.emplace(shown.items[index].game, shown.items[index].pickId);
    }
    for (std::size_t index = 0; index < shown.freeSlots.size(); ++index) {
        const world::FreeSlot& free = shown.freeSlots[index];
        if (room->m_items.contains(free.pickId)) {
            continue; // never both (the layout lists a slot once); kept safe all the same
        }
        room->m_free.emplace(free.pickId, index);
        targets.push_back(free.pickTarget());
    }
    auto index = world::PickIndex::build(targets, shown.occluders);
    if (!index) {
        return propagate(index.error());
    }
    room->m_index = std::move(*index);
    // Every slot that holds an item or is free is a step's stop (the layout's runs name each slot's pick id).
    std::vector<world::NavRun> runs = shown.navigation;
    for (world::NavRun& run : runs) {
        for (world::NavFixture& fixture : run.fixtures) {
            for (world::NavLevel& level : fixture.levels) {
                for (world::NavSlot& slot : level.slots) {
                    const bool stop = slot.slot != world::kNoPick &&
                                      (room->m_items.contains(slot.slot) || room->m_free.contains(slot.slot));
                    slot.item = stop ? slot.slot : world::kNoPick;
                }
            }
        }
    }
    auto navigation = world::SlotNavigation::build(std::move(runs));
    if (!navigation) {
        return propagate(navigation.error());
    }
    room->m_navigation = std::move(*navigation);
    return room;
}

const world::CollectionItem* EditRoom::item(world::PickId pick) const noexcept {
    const auto found = m_items.find(pick);
    return found != m_items.end() ? &m_layout->items[found->second] : nullptr;
}

const world::FreeSlot* EditRoom::freeSlot(world::PickId pick) const noexcept {
    const auto found = m_free.find(pick);
    return found != m_free.end() ? &m_layout->freeSlots[found->second] : nullptr;
}

std::optional<EditTarget> EditRoom::target(world::PickId pick) const {
    if (const world::CollectionItem* placed = item(pick)) {
        return EditTarget{pick, placed->slot, placed->game, placed->stance == world::ItemStance::FaceOut, false};
    }
    if (const world::FreeSlot* free = freeSlot(pick)) {
        return EditTarget{pick, free->slot, std::nullopt, free->stance == world::ItemStance::FaceOut, free->dressed};
    }
    return std::nullopt;
}

world::PickId EditRoom::pickOf(const catalog::GameId& game) const noexcept {
    const auto found = m_byGame.find(game);
    return found != m_byGame.end() ? found->second : world::kNoPick;
}

std::optional<world::PickId> EditRoom::step(world::PickId from, world::StepDirection direction) const noexcept {
    return m_navigation.step(from, direction);
}

std::optional<world::CollectionItem> EditRoom::asItem(world::PickId pick) const {
    if (const world::CollectionItem* placed = item(pick)) {
        return *placed;
    }
    const world::FreeSlot* free = freeSlot(pick);
    if (free == nullptr) {
        return std::nullopt;
    }
    world::CollectionItem standIn;
    standIn.pickId = free->pickId;
    standIn.slot = free->slot;
    standIn.section = free->section;
    standIn.stance = free->stance;
    standIn.cluster = free->cluster;
    standIn.slotFrame = free->slotFrame;
    standIn.slotBox = free->slotBox;
    return standIn;
}

std::optional<scene::WalkerPose> EditRoom::focusPose(world::PickId pick, const FocusViewer& viewer) const {
    const std::optional<world::CollectionItem> standIn = asItem(pick);
    if (!standIn) {
        return std::nullopt;
    }
    return focusPoseFor(*standIn, viewer);
}

} // namespace rv::app
