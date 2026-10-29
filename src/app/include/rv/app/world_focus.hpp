#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// The room's side of the hover, the pull-out and Focus (ADR-0012 §2, §3; I3 step 13a), between the collection's layout
// (world::CollectionLayout, step 10a) and app::WorldInteraction (step 7), tested without a window:
//  - CollectionRoom: the layout as the interaction's InteractionRoom: its items by pick id (face-out from the stance,
//    the section the item's fixture), each item's Focus pose and the D-pad steps of the layout's navigation runs;
//  - pulledItemTransform(): where a pulled item stands at a pull's progress: a spine-out cartridge slides out of its
//    row and turns its front (its cover) to the viewer, upright, a face-out one tips its top towards the viewer, a
//    cabinet's screen stays in its bezel (the fix of 2026-10-29);
//  - focusPoseFor(): the walker 0.9 m in front of the item, the reticle on the middle of its face;
//  - FocusStepRepeat: a held D-pad direction or arrow key in Focus steps again after a delay, as the quick catalog's
//    lists do (ui::DirectionRepeater's cadence);
//  - pickRay(): the frame's pick ray from the reticle or from the cursor (pickRaySource());
//  - itemBounds() and labelAnchor(): where the hover's title label stands, above the item as the view shows it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/world_interaction.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/viewpoint.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/slot_navigation.hpp>

namespace rv::app {

/// A spine-out item slides out of its row by this share of its depth (the slot's along +Z: ADR-0012 §3's 70%).
inline constexpr float kPullOutDepthShare = 0.7f;
/// A face-out item tips its top towards the viewer by this much, about its slot's bottom front edge (degrees).
inline constexpr float kFaceOutTiltDeg = 10.0f;
/// While a spine-out item turns, it stays this far in front of its row (meters): it never passes through its
/// neighbours.
inline constexpr float kPullClearance = 0.002f;
/// The most a row reaches past an item's face that rowReach() counts (meters): what stands farther out (furniture in
/// the room) is no part of the row.
inline constexpr float kMaxRowReach = 0.03f;
/// The share of a spine-out pull spent sliding straight out before the item starts to turn.
inline constexpr float kPullSlideShare = 0.4f;
/// Focus: the eye this far in front of the item's visible face, horizontally (ADR-0012 §3's 0.9 m).
inline constexpr float kFocusDistance = 0.9f;

/// How far the row a spine-out item turns in front of reaches past the item's face (meters, along its slot's +Z, from
/// 0 to kMaxRowReach): the fronts of its neighbours in its cluster (their insets differ) and of the occluders beside it
/// (a module's side panel at the end of a row) within the reach of its turn and its height; boards it stands on or
/// under do not count, but for an item that rolls upright as it turns (a cartridge on its end), whose corners sweep
/// past them (the fix of 2026-10-29). 0 for a face-out item, which only tips.
[[nodiscard]] float rowReach(const world::CollectionItem& item, const world::CollectionLayout& layout) noexcept;

/// The pose of a pulled item at `progress` (0 in its slot, 1 fully pulled; clamped), from the item prefab's space to
/// the room's frame, as item.transform is at 0; `reach` is its rowReach():
///  - spine-out (ADR-0012 §3): it slides straight out along its slot's +Z until its back clears its row (its face plus
///    `reach`) by kPullClearance (the first kPullSlideShare of the progress), then turns a quarter about the slot's +Y
///    through its centre, its front (its cover: the slot's +X while it stands spine-out) to the slot's +Z, rolling
///    meanwhile about the slot's +Z so that the cover stands upright (a cartridge on its end; the fix of 2026-10-29:
///    its cover showed sideways), while it settles to kPullOutDepthShare of its depth out of its slot: never closer to
///    the row than the clearance its turned footprint needs, so that it never passes through its neighbours or the fixture (an item at the end of a row whose
///    turned footprint would reach a side panel standing out settles that much farther out);
///  - face-out: it tips its top towards the viewer by kFaceOutTiltDeg about its slot box's bottom front edge;
///  - part of its fixture (CollectionItem::inFixture: an arcade cabinet's screen; the fix of 2026-10-29): it stays as
///    it stands, item.transform at every progress (a screen tipped out of its bezel read as the cabinet moving).
/// A function of the progress alone: an interrupted pull goes back the way it came.
[[nodiscard]] glm::mat4 pulledItemTransform(const world::CollectionItem& item, float progress,
                                            float reach = 0.0f) noexcept;

/// The item's box (its slot box, at `progress` of its pull) in the room's frame.
[[nodiscard]] core::Aabb itemBounds(const world::CollectionItem& item, float progress, float reach = 0.0f) noexcept;

/// The middle of the item's visible face (the slot box's +Z face) and its outward horizontal direction, in the room.
struct ItemFace {
    glm::vec3 center{0.0f};
    glm::vec3 out{0.0f, 0.0f, 1.0f};
};
[[nodiscard]] ItemFace itemFace(const world::CollectionItem& item) noexcept;

/// Where the walker looks from in Focus: the floor it stands on and its eye's height, the camera's lens and the view's
/// aspect, the part of the view the item is framed in (the whole view in Focus; the card's free part from step 14a).
struct FocusViewer {
    float floorY = 0.0f;
    float eyeHeight = 1.65f;
    scene::CameraLens lens;
    float aspect = 16.0f / 9.0f;
    scene::ViewArea area;
};

/// The walker's Focus pose on `item`: its feet on the viewer's floor, its eye kFocusDistance in front of the item's
/// visible face (horizontally, along itemFace().out), looking at the middle of that face in the viewer's area
/// (scene::aimAt): with the whole view the reticle rests on the item's pick target (its static face, which the pulled
/// item stands just in front of), so the hover stays on the focused item while the stick is still.
[[nodiscard]] scene::WalkerPose focusPoseFor(const world::CollectionItem& item, const FocusViewer& viewer) noexcept;

/// The collection's layout as WorldInteraction's room (ADR-0012 §1): an item per pick id (its GameId, face-out from its
/// stance, its section its fixture: the pick id's socket, so that looking in Focus moves within one module, display
/// case or wall shelf), its Focus pose (focusPoseFor() with the viewer set last) and the D-pad steps of the layout's
/// navigation runs (world::SlotNavigation). Main thread (the viewer changes between frames).
class CollectionRoom final : public InteractionRoom {
public:
    /// Errors: those of world::SlotNavigation::build() for the layout's runs.
    [[nodiscard]] static Expected<std::unique_ptr<CollectionRoom>> build(
        std::shared_ptr<const world::CollectionLayout> layout);

    [[nodiscard]] std::optional<RoomItem> item(world::PickId id) const override;
    [[nodiscard]] scene::WalkerPose focusPose(world::PickId id) const override;
    [[nodiscard]] std::optional<world::PickId> step(world::PickId from,
                                                    world::StepDirection direction) const override;

    /// The viewer the next focusPose() calls frame for.
    void setViewer(const FocusViewer& viewer) noexcept { m_viewer = viewer; }
    [[nodiscard]] const FocusViewer& viewer() const noexcept { return m_viewer; }
    /// The layout's item of `id`, or nullptr.
    [[nodiscard]] const world::CollectionItem* find(world::PickId id) const noexcept;
    /// The rowReach() of the item of `id` (0 for an unknown one), computed once.
    [[nodiscard]] float reachOf(world::PickId id) const noexcept;
    [[nodiscard]] const world::CollectionLayout& layout() const noexcept { return *m_layout; }

    /// The section of an item: its fixture's socket (the pick id's top byte, world::slotPickId()).
    [[nodiscard]] static constexpr std::uint32_t sectionOf(world::PickId id) noexcept { return id >> 24; }

private:
    CollectionRoom() = default;

    std::shared_ptr<const world::CollectionLayout> m_layout;
    std::map<world::PickId, std::size_t> m_byPick;
    /// rowReach() of each item, in item order.
    std::vector<float> m_reach;
    world::SlotNavigation m_navigation;
    FocusViewer m_viewer;
};

/// The D-pad and the arrow keys in Focus (I3 step 13a's choice; step 7 steps once per press): a direction held steps at
/// once, again after ui::NavSettings::repeatDelay, then every repeatInterval (fastRepeatInterval once held for
/// fastRepeatAfter), at most one step a frame and no backlog, as the quick catalog's lists repeat; a direction pressed
/// and released within a frame steps once; among held directions the one pressed last wins. Deterministic on the frame
/// times it is given.
class FocusStepRepeat {
public:
    explicit FocusStepRepeat(const ui::NavSettings& settings = {}) noexcept;

    /// `actions` with the presses of ui_up, ui_down, ui_left and ui_right replaced by this frame's step (one of them
    /// pressed, the others not; held and released as they are). Call it on every frame in Focus, reset() otherwise.
    [[nodiscard]] input::ActionState apply(const input::ActionState& actions, double deltaSeconds);
    /// Forgets the held direction and the press order: a direction still held steps only when pressed again (entering
    /// Focus with an arrow held from walking does not step).
    void reset() noexcept;

private:
    ui::DirectionRepeater m_repeater;
    /// Order of the last press of each direction (Up, Down, Left, Right; 0 never), from m_pressCounter.
    std::array<std::uint64_t, 4> m_pressOrder{};
    std::uint64_t m_pressCounter = 0;
    /// Set by reset(): held directions give no step until released or pressed again.
    bool m_suppressHeld = true;
};

/// The frame's pick ray (ADR-0012 §2): from the view's centre (Reticle), or through `cursor` (window coordinates, y
/// down, in a window of `windowSize` in the same units; the view's centre for an empty window) with the camera's
/// aspect set to the window's.
[[nodiscard]] core::Ray pickRay(const scene::Camera& camera, PickRaySource source, glm::vec2 cursor,
                                glm::vec2 windowSize) noexcept;

/// Where the hover's title label stands in an output of `size` pixels (y down): the middle of the top edge of
/// `bounds` as `viewProjection` shows it, its height clamped to at least `topMargin` pixels from the top; nothing when
/// the box is off screen, reaches behind the eye, is empty or not finite.
[[nodiscard]] std::optional<glm::vec2> labelAnchor(const core::Aabb& bounds, const glm::mat4& viewProjection,
                                                   glm::ivec2 size, float topMargin) noexcept;

} // namespace rv::app
