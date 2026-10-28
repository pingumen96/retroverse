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
//
// I4b step 4 adds the room's movable fixtures (world::editableFixtures(): side-wall modules, display fixtures,
// decoration), targeted by the reticle where no game or slot is (fixtureAt(): their boxes in the scene's frame, the
// nearest within kFixturePickDistance, unless a fixed fixture's box, the hero corner's or a hero-wall module's:
// fixedFixtureBoxes(), stands before it), and what a held fixture follows: the floor point under the reticle, or for a
// hung decoration the wall point (in the room's frame, millimetres: floorAim(), wallAim()). While a game is held the
// reticle passes the dressing (heldIndex(): the occluders without the dressing props', which give way where a game
// lands), so that a dressed free slot is targeted from afar too.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/world_focus.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/math.hpp>
#include <rv/scene/viewpoint.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/slot_navigation.hpp>
#include <rv/world/world_edit.hpp>

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
    /// I4b step 4: a movable fixture (pick kNoPick, no slot, no game).
    std::optional<world::FixtureId> fixture;

    /// An empty slot (neither a game nor a fixture).
    [[nodiscard]] bool free() const noexcept { return !game.has_value() && !fixture.has_value(); }
    bool operator==(const EditTarget&) const = default;
};

/// The edited room's frame and interior (I4b step 4): where its fixtures stand (millimetres in its frame) and how its
/// frame stands in the scene's.
struct EditRoomFrame {
    /// The room's frame to the scene's (world::roomToFloorMatrix() of its scene placement).
    glm::mat4 toScene{1.0f};
    /// Its interior in its frame (world::Room's).
    std::int64_t minXMm = 0;
    std::int64_t maxXMm = 0;
    std::int64_t minZMm = 0;
    std::int64_t maxZMm = 0;
    std::int64_t heightMm = 0;
};

/// How far the reticle reaches a fixture (meters): farther than a game (world::kMaxPickDistance), furniture being
/// large.
inline constexpr float kFixturePickDistance = 4.0f;

/// The boxes of `room`'s fixed fixtures in `plan` (the hero corner's parts, the hero-wall modules; world::fixtureBox(),
/// millimetres in the room's frame): what hides a movable fixture behind them from the reticle.
[[nodiscard]] std::vector<world::FixtureBox> fixedFixtureBoxes(const world::WorldPlan& plan, const world::RoomId& room);

/// Where a fixture standing at `place` is drawn relative to where it stood at `from`, in the frame `toScene` maps the
/// room's into: the move about +Y and along the floor (millimetres and centidegrees in the room's frame).
[[nodiscard]] glm::mat4 fixtureMove(const world::FixtureTransform& from, const world::FixtureTransform& place,
                                    const glm::mat4& toScene) noexcept;

class EditRoom {
public:
    /// `fixed`: the fixed fixtures' boxes (fixedFixtureBoxes()), which hide the movable ones behind them. Errors:
    /// those of world::PickIndex::build() and world::SlotNavigation::build() for the layout.
    [[nodiscard]] static Expected<std::unique_ptr<EditRoom>>
    build(std::shared_ptr<const world::CollectionLayout> layout, std::vector<world::EditableFixture> fixtures = {},
          const EditRoomFrame& frame = {}, std::span<const world::FixtureBox> fixed = {});

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
    /// The same targets with the occluders but the dressing props' (a held game's ray: the dressing gives way).
    [[nodiscard]] const world::PickIndex& heldIndex() const noexcept { return m_heldIndex; }

    /// The room's movable fixtures (I4b step 4), the one of `id` (nullptr: none), its target.
    [[nodiscard]] const std::vector<world::EditableFixture>& fixtures() const noexcept { return m_fixtures; }
    [[nodiscard]] const world::EditableFixture* fixture(const world::FixtureId& id) const noexcept;
    [[nodiscard]] std::optional<EditTarget> fixtureTarget(const world::FixtureId& id) const;
    /// The nearest movable fixture whose box `ray` (the scene's frame) enters within `maxDistance`, if any; none when
    /// the ray enters a fixed fixture's box before it.
    [[nodiscard]] std::optional<world::FixtureId> fixtureAt(const core::Ray& ray,
                                                            float maxDistance = kFixturePickDistance) const noexcept;
    /// Where `ray` (the scene's frame) meets the room's floor, in the room's frame (millimetres; the yaw 0), kept
    /// within its interior; nothing when it does not go down to it.
    [[nodiscard]] std::optional<world::FixtureTransform> floorAim(const core::Ray& ray) const noexcept;
    /// Where `ray` (from inside the room) meets its walls, in the room's frame (millimetres; the yaw 0); nothing when
    /// it meets none.
    [[nodiscard]] std::optional<world::FixtureTransform> wallAim(const core::Ray& ray) const noexcept;
    /// `direction` (the scene's frame) on the room's floor, in the room's frame (x, z; not normalised).
    [[nodiscard]] glm::vec2 roomDirection(const glm::vec3& direction) const noexcept;
    [[nodiscard]] const EditRoomFrame& frame() const noexcept { return m_frame; }
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
    world::PickIndex m_heldIndex;
    std::vector<world::EditableFixture> m_fixtures;
    /// Their boxes in the scene's frame, and the fixed fixtures'.
    std::vector<core::Aabb> m_fixtureBoxes;
    std::vector<core::Aabb> m_fixedBoxes;
    EditRoomFrame m_frame;
    world::SlotNavigation m_navigation;
    /// Indices into the layout's items and free slots, by pick id.
    std::map<world::PickId, std::size_t> m_items;
    std::map<world::PickId, std::size_t> m_free;
    std::map<catalog::GameId, world::PickId> m_byGame;
};

} // namespace rv::app
