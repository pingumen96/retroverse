#pragma once

// The hover, the pull-out and Focus in the app (I3 step 13a; ADR-0012 §1-§3): app::WorldInteraction (step 7) driven
// over the room's collection every frame.
//  - Picking: the frame's ray (the reticle, or the cursor while the mouse is free: pickRaySource()) sampled against
//    the room's world::PickIndex with the current hover; the interaction's hysteresis decides the hover.
//  - The hover's outline: the highlighted item's records flagged highlighted (CollectionInstance::setHovered: the
//    outline pass of step 8), on its static records or on its dynamic copy while it is pulled; its title label above
//    it (labelFor(), the product UI's WorldLabelPanel).
//  - The pull-out on the dynamic path: every item off its static pose (WorldInteraction::pulledItems()) has its static
//    records flagged hidden and is drawn as render items at pulledItemTransform() (viewItems()), its layers and its
//    highlight as its records have them; back in its slot, its records are drawn again.
//  - Focus: the walker eased to focusPoseFor() (the interaction's result places it), the D-pad steps of the layout's
//    navigation, repeated while held (FocusStepRepeat).
//  - The world's Back levels and the quick catalog in the World context (InteractionResult::shellBack and openCatalog,
//    which the application performs), the "animation" phase while an animation runs, the run report's
//    world.interaction record.
// Inspect (I3 step 13b, WorldInspection): the focused item lifted from its pulled-out pose into the hand, turned and
// zoomed, the scene behind it dimmed. Avvia comes out as the result's launch, which the application asks of the product
// UI (I3 step 15a, world_launch.hpp: launchEnded() at its end; "Mostra nella stanza" is showInRoom()). Main thread
// only.
//
// Since I4 step 5a a floor has a collection per room (ADR-0013 §9): each room has its own pick index and D-pad steps
// (addRoom()), and the interaction works in the room that holds the eye (setActiveRoom(), every frame; none in a hall).
// The room changes only in free roam, once the room it leaves has let go of every item (nothing hovered, pulled or
// focused): until then the interaction stays with the room it is in.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <glm/vec2.hpp>

#include <rv/app/back_action.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/app/world_label_ui.hpp>
#include <rv/core/aabb.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/pick_index.hpp>

#include "world_inspection.hpp"

namespace rv::app {

class AppContext;
class CollectionInstance;

class WorldHover {
public:
    /// One frame as the application sees it.
    struct Frame {
        /// The frame's actions as resolved, or nullptr while the product UI has the input (InputContext::Ui).
        const input::ActionState* actions = nullptr;
        float deltaSeconds = 0.0f;
        /// The walker's pose as the frame starts.
        scene::WalkerPose walker;
        /// The frame's pick ray and where it came from; nothing while the product UI has the input.
        std::optional<core::Ray> ray;
        PickRaySource raySource = PickRaySource::Reticle;
        /// The shell's Back levels.
        BackState back;
        float wheelNotches = 0.0f;
        /// Where Focus poses are framed from (the walker's floor and eye, the camera's lens, the view's aspect).
        FocusViewer viewer;
        /// A button of the card pressed (I3 step 14a: WorldCard::press).
        std::optional<CardAction> card;
    };

    WorldHover(AppContext& context, InteractionSettings settings);
    ~WorldHover();
    WorldHover(const WorldHover&) = delete;
    WorldHover& operator=(const WorldHover&) = delete;

    /// A room's collection on the floor, committed at a frame boundary (`collection` must stay until clear()): its
    /// layout's items, navigation and pick targets with its occluders, as room `addRoom()`'s count so far. Returns the
    /// pick targets indexed (0 when the index or the navigation cannot be built: logged, the room then hovers nothing).
    /// The first room added is the active one until setActiveRoom() says otherwise.
    std::size_t addRoom(world::CollectionLayout layout, CollectionInstance* collection);
    /// The room that holds the eye (an index of addRoom()'s order; none: a hall, which hovers nothing). It becomes the
    /// interaction's room once the interaction is in free roam with nothing hovered, pulled or focused in the room it
    /// leaves (asked again every frame until then).
    void setActiveRoom(std::optional<std::size_t> room);
    /// The interaction's room now (none: no room, or a hall), and its id.
    [[nodiscard]] std::optional<std::size_t> activeRoom() const noexcept { return m_active; }
    [[nodiscard]] std::optional<world::RoomId> activeRoomId() const {
        return m_room != nullptr ? std::optional<world::RoomId>(m_room->layout().room) : std::nullopt;
    }
    /// The collections go (before the application replaces the scene): no room, nothing pulled.
    void clear();

    /// One frame (WorldInteraction::update over the room), its flags written to the collection. The result asks the
    /// application for a shell Back level, the quick catalog and the walker's pose of an ease.
    [[nodiscard]] InteractionResult update(const Frame& frame);

    /// The view's render items: `sceneItems` alone, or with the pulled items' parts after them, the inspected one in
    /// the hand in front of `camera` (WorldInspection::itemModel; the storage is this object's, valid until the next
    /// call).
    [[nodiscard]] std::span<const scene::RenderItem> viewItems(std::span<const scene::RenderItem> sceneItems,
                                                               const scene::Camera& camera);
    /// The inspection's dimming of the frame's view (WorldInspection::apply).
    void applyInspection(gl::RenderView& view) { m_inspection.apply(m_interaction, view); }
    /// The inspection (the held pose, the dimming, its record).
    [[nodiscard]] WorldInspection& inspection() noexcept { return m_inspection; }
    /// The GameId text of the item in Focus or Inspect (the hero CRT's selection, screenSelection()), "" for none.
    [[nodiscard]] std::string focusedGame() const;
    /// The label of the highlighted item as `camera` shows it in a view of `viewSize` pixels, if any.
    [[nodiscard]] std::optional<WorldLabelContent> labelFor(const scene::Camera& camera, glm::ivec2 viewSize);

    [[nodiscard]] world::PickId hovered() const noexcept { return m_interaction.hovered(); }
    [[nodiscard]] world::PickId highlighted() const noexcept { return m_interaction.highlighted(); }
    /// The item is off its static pose (drawn by viewItems()).
    [[nodiscard]] bool isPulled(world::PickId id) const noexcept {
        return id != world::kNoPick && std::binary_search(m_hidden.begin(), m_hidden.end(), id);
    }
    [[nodiscard]] const WorldInteraction& interaction() const noexcept { return m_interaction; }
    /// I3 step 15a: the launch of the result's `launch` ended (WorldInteraction::launchEnded); "Mostra nella stanza" on
    /// `game` (WorldInteraction::showInRoom from `walker`): the item's pick id, or kNoPick when the room shown has no
    /// item of it or the state takes none (the interaction's notice tells which); the pick id of `game`'s item.
    void launchEnded(const LaunchMessage& outcome) { m_interaction.launchEnded(outcome); }
    [[nodiscard]] world::PickId showInRoom(const catalog::GameId& game, const scene::WalkerPose& walker);
    [[nodiscard]] world::PickId pickOf(const catalog::GameId& game) const noexcept;
    /// The interaction's room's item `id`, or null (no room, no such item).
    [[nodiscard]] const world::CollectionItem* item(world::PickId id) const noexcept {
        return m_room != nullptr ? m_room->find(id) : nullptr;
    }
    /// The interaction's room's collection, or null.
    [[nodiscard]] CollectionInstance* collection() const noexcept { return m_collection; }
    /// Changes of the hovered item since the start.
    [[nodiscard]] std::uint64_t hoverChanges() const noexcept { return m_record.hoverChanges; }

    /// The end of the run: the interaction record (unbound when no collection ever showed).
    void record(InteractionRecord& record) const;

private:
    /// The highlight and the hidden flags of the collection's records as the interaction has them now.
    void syncCollection();
    /// The record's counts and events of this frame.
    void observe(bool stepped, bool fromCursor);
    void addEvent(std::string event, world::PickId id = world::kNoPick, std::string state = {});
    /// The "animation" phase open while an animation of the interaction runs.
    void syncAnimationPhase(bool animating);

    /// One collection room of the floor.
    struct RoomEntry {
        std::unique_ptr<CollectionRoom> room;
        world::PickIndex index;
        CollectionInstance* collection = nullptr;
    };

    /// Makes room `room` (none: no room) the interaction's, letting go of the one it leaves first.
    void bindRoom(std::optional<std::size_t> room);

    AppContext& m_context;
    WorldInteraction m_interaction;
    /// A deque: m_index points into an entry, which a later addRoom() must not move.
    std::deque<RoomEntry> m_rooms;
    /// The interaction's room (an index into m_rooms), the one asked for (setActiveRoom()), and its parts.
    std::optional<std::size_t> m_active;
    std::optional<std::size_t> m_wanted;
    CollectionRoom* m_room = nullptr;
    const world::PickIndex* m_index = nullptr;
    CollectionInstance* m_collection = nullptr;
    FocusStepRepeat m_repeat;
    /// The items whose static records are hidden (the pulled ones), ascending.
    std::vector<world::PickId> m_hidden;
    std::vector<scene::RenderItem> m_viewItems;
    bool m_animationOpen = false;
    /// What the last frame showed, to record the changes.
    WorldState m_lastState = WorldState::FreeRoam;
    world::PickId m_lastHovered = world::kNoPick;
    world::PickId m_lastFocused = world::kNoPick;
    std::vector<PulledItem> m_lastPulled;
    InteractionRecord m_record;
    WorldInspection m_inspection;
};

} // namespace rv::app
