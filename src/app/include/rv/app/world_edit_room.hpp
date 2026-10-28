#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// The edited room as the editor asks it (I4b step 3, ADR-0015 §4): the room's collection layout (world::
// CollectionLayout in the scene's frame, I4 step 5a) with its items and its free slots (world::FreeSlot: bare or
// dressed), so that
//  - the reticle picks both (one world::PickIndex over the items' targets and the free slots', with the layout's
//    occluders: a free slot's target is its slot volume with the visible face inflated, as an item's);
//  - the D-pad steps over every slot that holds an item or is free (Focus's steps, world::SlotNavigation over the
//    layout's runs, empty slots included: NavSlot::slot);
//  - each target has a Focus-like pose (focusPoseFor(): the walker 0.9 m in front of its face, the reticle on it).
// A pick id is a slot's (world::slotPickId()): an item and the free slot it would leave share the id across a move.

#include <map>
#include <memory>
#include <optional>

#include <rv/app/world_focus.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/scene/viewpoint.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/slot_navigation.hpp>

namespace rv::app {

/// What the editor's reticle or D-pad rests on.
struct EditTarget {
    world::PickId pick = world::kNoPick;
    world::SlotId slot;
    /// The game in the slot; none for a free slot.
    std::optional<catalog::GameId> game;
    /// The slot shows its item face-out (a display case, the featured shelf, the band).
    bool faceOut = false;
    /// A free slot a dressed level's span covers (its dressing gives way where a game lands).
    bool dressed = false;

    [[nodiscard]] bool free() const noexcept { return !game.has_value(); }
    bool operator==(const EditTarget&) const = default;
};

class EditRoom {
public:
    /// Errors: those of world::PickIndex::build() and world::SlotNavigation::build() for the layout.
    [[nodiscard]] static Expected<std::unique_ptr<EditRoom>> build(std::shared_ptr<const world::CollectionLayout> layout);

    /// The target of `pick` (an item or a free slot of the room), or nothing.
    [[nodiscard]] std::optional<EditTarget> target(world::PickId pick) const;
    /// The pick id of `game`'s item, or kNoPick.
    [[nodiscard]] world::PickId pickOf(const catalog::GameId& game) const noexcept;
    /// The step from `from` (an item or a free slot) towards `direction` over every slot that holds an item or is free.
    [[nodiscard]] std::optional<world::PickId> step(world::PickId from, world::StepDirection direction) const noexcept;
    /// The walker's pose in front of `pick` (focusPoseFor() of its item, or of a free slot as an item would stand
    /// there); nothing for a pick the room lacks.
    [[nodiscard]] std::optional<scene::WalkerPose> focusPose(world::PickId pick, const FocusViewer& viewer) const;

    /// The items' and the free slots' targets with the layout's occluders.
    [[nodiscard]] const world::PickIndex& index() const noexcept { return m_index; }
    [[nodiscard]] const world::CollectionLayout& layout() const noexcept { return *m_layout; }
    [[nodiscard]] const world::RoomId& room() const noexcept { return m_layout->room; }
    /// The layout's item of `pick`, or nullptr; the free slot of `pick`, or nullptr.
    [[nodiscard]] const world::CollectionItem* item(world::PickId pick) const noexcept;
    [[nodiscard]] const world::FreeSlot* freeSlot(world::PickId pick) const noexcept;
    /// `pick`'s item, or the item a free slot would hold (its frame and box, its stance; no game).
    [[nodiscard]] std::optional<world::CollectionItem> asItem(world::PickId pick) const;

private:
    EditRoom() = default;

    std::shared_ptr<const world::CollectionLayout> m_layout;
    world::PickIndex m_index;
    world::SlotNavigation m_navigation;
    /// Indices into the layout's items and free slots, by pick id.
    std::map<world::PickId, std::size_t> m_items;
    std::map<world::PickId, std::size_t> m_free;
    std::map<catalog::GameId, world::PickId> m_byGame;
};

} // namespace rv::app
