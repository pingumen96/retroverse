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
// Inspect is entered as the model says (a face-out item, interact in Focus) but drawn by I3 step 13b: until then the
// item stays at its pulled-out pose, and Avvia ends at once as if the game had returned (launching from the room is
// I3 step 15a's). Main thread only.

#include <algorithm>
#include <cstdint>
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

    /// The room's collection, committed at a frame boundary (`collection` must stay until clear() or the next
    /// setRoom()): its layout's items, navigation and pick targets with its occluders. Returns the pick targets indexed
    /// (0 when the index or the navigation cannot be built: logged, the room then hovers nothing).
    std::size_t setRoom(world::CollectionLayout layout, CollectionInstance* collection);
    /// The collection goes (before the application replaces the scene): no room, nothing pulled.
    void clear();

    /// One frame (WorldInteraction::update over the room), its flags written to the collection. The result asks the
    /// application for a shell Back level, the quick catalog and the walker's pose of an ease.
    [[nodiscard]] InteractionResult update(const Frame& frame);

    /// The view's render items: `sceneItems` alone, or with the pulled items' parts after them (the storage is this
    /// object's, valid until the next call).
    [[nodiscard]] std::span<const scene::RenderItem> viewItems(std::span<const scene::RenderItem> sceneItems);
    /// The label of the highlighted item as `camera` shows it in a view of `viewSize` pixels, if any.
    [[nodiscard]] std::optional<WorldLabelContent> labelFor(const scene::Camera& camera, glm::ivec2 viewSize);

    [[nodiscard]] world::PickId hovered() const noexcept { return m_interaction.hovered(); }
    [[nodiscard]] world::PickId highlighted() const noexcept { return m_interaction.highlighted(); }
    /// The item is off its static pose (drawn by viewItems()).
    [[nodiscard]] bool isPulled(world::PickId id) const noexcept {
        return id != world::kNoPick && std::binary_search(m_hidden.begin(), m_hidden.end(), id);
    }
    [[nodiscard]] const WorldInteraction& interaction() const noexcept { return m_interaction; }
    /// The room's item `id`, or null (no room, no such item).
    [[nodiscard]] const world::CollectionItem* item(world::PickId id) const noexcept {
        return m_room ? m_room->find(id) : nullptr;
    }
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

    AppContext& m_context;
    WorldInteraction m_interaction;
    std::unique_ptr<CollectionRoom> m_room;
    world::PickIndex m_index;
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
};

} // namespace rv::app
