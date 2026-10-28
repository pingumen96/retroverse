#include "world_hover.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/app/frame_timing.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/input/action.hpp>
#include <rv/scene/instance_data.hpp>

#include "collection_instance.hpp"

namespace rv::app {
namespace {

[[nodiscard]] std::string pickText(world::PickId id) {
    return id == world::kNoPick ? std::string() : fmt::format("{:08X}", id);
}

[[nodiscard]] bool arrowPressed(const input::ActionState& actions) noexcept {
    using input::Action;
    return actions.pressed(Action::UiUp) || actions.pressed(Action::UiDown) || actions.pressed(Action::UiLeft) ||
           actions.pressed(Action::UiRight);
}

} // namespace

WorldHover::WorldHover(AppContext& context, InteractionSettings settings)
    : m_context(context), m_interaction(settings) {
    m_record.reduceMotion = settings.reduceMotion;
}

WorldHover::~WorldHover() {
    syncAnimationPhase(false);
}

std::size_t WorldHover::addRoom(world::CollectionLayout layout, CollectionInstance* collection) {
    const auto start = std::chrono::steady_clock::now();
    const std::string roomId = layout.room.toString();
    RoomEntry entry;
    entry.collection = collection;
    auto index = world::PickIndex::build(layout.pickTargets(), layout.occluders);
    if (!index) {
        spdlog::error("World: the pick index of {} cannot be built: {}", roomId, index.error().toString());
    } else if (auto room = CollectionRoom::build(std::make_shared<const world::CollectionLayout>(std::move(layout)))) {
        entry.index = std::move(*index);
        entry.room = std::move(*room);
    } else {
        spdlog::error("World: the D-pad steps of {} cannot be built: {}", roomId, room.error().toString());
    }
    const std::size_t targets = entry.room ? entry.index.targetCount() : 0;
    if (entry.room) {
        spdlog::info("World: the hover, the pull-out and Focus over {} item(s) of {} ({} pick target(s), {} "
                     "occluder(s)), ready in {:.1f} ms",
                     entry.room->layout().items.size(), roomId, targets, entry.room->layout().occluders.size(),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    m_rooms.push_back(std::move(entry));
    m_record.bound = true;
    if (m_rooms.size() == 1) {
        m_wanted = 0;
        bindRoom(0);
    }
    return targets;
}

void WorldHover::setActiveRoom(std::optional<std::size_t> room) {
    if (room && *room >= m_rooms.size()) {
        room.reset();
    }
    m_wanted = room;
    if (room == m_active) {
        return;
    }
    // The interaction leaves its room only in free roam once it has let go of every item of it: nothing hovered,
    // pulled or focused (the walker cannot walk out of Focus or Inspect; a pulled item first goes back to its slot).
    if (m_interaction.state() != WorldState::FreeRoam || m_interaction.hovered() != world::kNoPick ||
        !m_interaction.pulledItems().empty()) {
        return;
    }
    bindRoom(room);
}

void WorldHover::bindNow(std::optional<std::size_t> room) {
    if (room && *room >= m_rooms.size()) {
        room.reset();
    }
    m_interaction.leaveFocusNow();
    m_wanted = room;
    if (room != m_active) {
        bindRoom(room);
    }
}

void WorldHover::bindRoom(std::optional<std::size_t> room) {
    // The room left lets go of its items (none hovered, focused or pulled; its records' flags as they stand: synced),
    // a frame of no time absorbing what the interaction would do with it.
    if (m_room != nullptr) {
        m_interaction.setRoom(nullptr);
        static_cast<void>(m_interaction.update(InteractionFrame{}));
        syncCollection();
    }
    m_active = room;
    const RoomEntry* entry = room ? &m_rooms[*room] : nullptr;
    m_room = entry != nullptr && entry->room ? entry->room.get() : nullptr;
    m_index = m_room != nullptr ? &entry->index : nullptr;
    m_collection = m_room != nullptr ? entry->collection : nullptr;
    m_hidden.clear();
    m_lastPulled.clear();
    m_interaction.setRoom(m_room);
    if (m_room != nullptr && m_rooms.size() > 1) {
        spdlog::info("Frame {}: the interaction is in {}", m_context.frameIndex(), m_room->layout().room.toString());
    }
}

void WorldHover::clear() {
    // The interaction lets go of the items first (none hovered, focused or pulled), then the rooms go. A walker's ease
    // it stops there would place the walker at its next update, in a scene that may be gone: a frame of no time, its
    // result dropped, absorbs it (the application places the walker in the new scene itself).
    m_interaction.setRoom(nullptr);
    static_cast<void>(m_interaction.update(InteractionFrame{}));
    m_collection = nullptr;
    m_room = nullptr;
    m_index = nullptr;
    m_rooms.clear();
    m_active.reset();
    m_wanted.reset();
    m_hidden.clear();
    m_viewItems.clear();
    m_lastPulled.clear();
    m_lastHovered = world::kNoPick;
    m_lastFocused = world::kNoPick;
    m_lastState = m_interaction.state();
    syncAnimationPhase(false);
}

bool WorldHover::enterEdit() {
    if (!m_interaction.enterEdit()) {
        return false;
    }
    syncCollection();
    syncAnimationPhase(false);
    addEvent("edit", world::kNoPick, std::string(toString(m_interaction.state())));
    m_lastState = m_interaction.state();
    m_lastFocused = world::kNoPick;
    m_lastHovered = world::kNoPick;
    m_lastPulled.clear();
    return true;
}

void WorldHover::leaveEdit() {
    m_interaction.leaveEdit();
    syncCollection();
    addEvent("edit-end", world::kNoPick, std::string(toString(m_interaction.state())));
    m_lastState = m_interaction.state();
}

InteractionResult WorldHover::update(const Frame& frame) {
    // The room that holds the eye, once the room the interaction is in let go of its items.
    if (m_wanted != m_active) {
        setActiveRoom(m_wanted);
    }
    if (m_room != nullptr) {
        m_room->setViewer(frame.viewer);
    }
    InteractionFrame input;
    input.deltaSeconds = frame.deltaSeconds;
    input.walker = frame.walker;
    input.back = frame.back;
    input.wheelNotches = frame.wheelNotches;
    input.card = frame.card;
    // In Focus a held arrow or D-pad direction repeats its step (FocusStepRepeat); elsewhere it does not step.
    input::ActionState stepped;
    if (frame.actions != nullptr && m_interaction.context() == WorldContext::Focus) {
        stepped = m_repeat.apply(*frame.actions, frame.deltaSeconds);
        input.actions = &stepped;
    } else {
        m_repeat.reset();
        input.actions = frame.actions;
    }
    const bool fromCursor = m_room != nullptr && frame.ray && frame.raySource == PickRaySource::Cursor;
    if (m_room != nullptr && m_index != nullptr && frame.ray) {
        // The eye left the interaction's room: its items are hovered no more (a miss), so that it can let go of them.
        input.hover = m_wanted == m_active ? m_index->sample(*frame.ray, m_interaction.hovered()) : world::HoverSample{};
    }
    const WorldState before = m_interaction.state();
    InteractionResult result = m_interaction.update(input);
    m_inspection.observe(m_interaction, before, result);
    // Avvia (result.launch) goes to the product UI through the application (I3 step 15a, WorldLaunch).
    if (result.showMedia || result.showInfo) {
        spdlog::debug("{} in the room: I3 step 14b", result.showMedia ? "Vedi media" : "Informazioni");
    }
    syncCollection();
    observe(input.actions == &stepped && arrowPressed(stepped), fromCursor);
    return result;
}

world::PickId WorldHover::pickOf(const catalog::GameId& game) const noexcept {
    if (m_room == nullptr) {
        return world::kNoPick;
    }
    const world::CollectionItem* item = m_room->layout().findGame(game);
    return item != nullptr ? item->pickId : world::kNoPick;
}

world::PickId WorldHover::showInRoom(const catalog::GameId& game, const scene::WalkerPose& walker) {
    const world::PickId id = pickOf(game);
    if (id == world::kNoPick || !m_interaction.showInRoom(id, walker)) {
        return world::kNoPick;
    }
    syncCollection();
    return id;
}

void WorldHover::syncCollection() {
    if (m_collection == nullptr) {
        return;
    }
    std::vector<world::PickId> pulled;
    for (const PulledItem& item : m_interaction.pulledItems()) {
        pulled.push_back(item.id); // ascending, as pulledItems() lists them
    }
    for (const world::PickId id : m_hidden) {
        if (!std::binary_search(pulled.begin(), pulled.end(), id)) {
            if (Status shown = m_collection->setHidden(id, false); !shown) {
                spdlog::error("World: item {} back in its slot: {}", pickText(id), shown.error().toString());
            }
        }
    }
    for (const world::PickId id : pulled) {
        if (!std::binary_search(m_hidden.begin(), m_hidden.end(), id)) {
            if (Status hidden = m_collection->setHidden(id, true); !hidden) {
                spdlog::error("World: item {} off its slot: {}", pickText(id), hidden.error().toString());
            }
        }
    }
    m_hidden = std::move(pulled);
    // The outline's mark: the highlighted item, else the item in the hand (the inspection's dimming, I3 step 13b).
    if (Status set = m_collection->setHovered(WorldInspection::markedItem(m_interaction)); !set) {
        spdlog::error("World: the hover outline: {}", set.error().toString());
    }
}

void WorldHover::addEvent(std::string event, world::PickId id, std::string state) {
    if (m_record.events.size() >= kMaxInteractionEvents) {
        ++m_record.eventsDropped;
        return;
    }
    m_record.events.push_back(
        InteractionEventRecord{m_context.frameIndex(), std::move(event), pickText(id), std::move(state)});
}

void WorldHover::observe(bool stepped, bool fromCursor) {
    const WorldState state = m_interaction.state();
    const world::PickId hovered = m_interaction.hovered();
    const world::PickId focused = m_interaction.focused();
    const std::span<const PulledItem> pulled = m_interaction.pulledItems();
    const std::uint64_t frame = m_context.frameIndex();

    if (state != m_lastState) {
        addEvent("state", world::kNoPick, std::string(toString(state)));
        spdlog::info("Frame {}: world {} -> {}{}", frame, toString(m_lastState), toString(state),
                     focused == world::kNoPick ? std::string() : " on " + pickText(focused));
    }
    if (hovered != m_lastHovered) {
        ++m_record.hoverChanges;
        addEvent("hover", hovered);
        spdlog::debug("Frame {}: hover {}", frame, hovered == world::kNoPick ? std::string("none") : pickText(hovered));
    }
    if (focused != m_lastFocused && focused != world::kNoPick && state == WorldState::Focus &&
        m_lastState == WorldState::Focus) {
        ++m_record.focusChanges;
        addEvent("focus", focused);
        if (stepped) {
            ++m_record.steps;
            addEvent("step", focused);
        }
    }
    for (const PulledItem& item : pulled) {
        const auto before = std::find_if(m_lastPulled.begin(), m_lastPulled.end(),
                                         [&](const PulledItem& last) { return last.id == item.id; });
        if (before == m_lastPulled.end()) {
            ++m_record.pullOuts;
            addEvent("pull-out", item.id);
        }
        if (item.progress >= 1.0f && (before == m_lastPulled.end() || before->progress < 1.0f)) {
            addEvent("pulled", item.id);
        }
    }
    for (const PulledItem& last : m_lastPulled) {
        if (std::none_of(pulled.begin(), pulled.end(), [&](const PulledItem& item) { return item.id == last.id; })) {
            addEvent("in", last.id);
        }
    }

    // No static record drawn for an item off its pose, and no record hidden without its dynamic copy.
    if (!pulled.empty()) {
        ++m_record.pulledFrames;
        m_record.maxPulled = std::max<std::uint64_t>(m_record.maxPulled, pulled.size());
    }
    if (m_collection != nullptr && (!pulled.empty() || !m_lastPulled.empty())) {
        bool mismatch = m_collection->hiddenItems() != m_hidden;
        for (const PulledItem& item : pulled) {
            mismatch = mismatch || m_collection->staticDrawn(item.id) || m_room->find(item.id) == nullptr;
        }
        m_record.staticDrawnWhilePulledFrames += mismatch ? 1 : 0;
    }

    const bool pulling = std::any_of(pulled.begin(), pulled.end(),
                                     [](const PulledItem& item) {
                                         return item.progress > 0.0f && item.progress < 1.0f;
                                     });
    const float lift = m_interaction.inspectView().lift;
    const bool animating = m_interaction.walkerEasing() || pulling || (lift > 0.0f && lift < 1.0f);
    syncAnimationPhase(animating);
    m_record.animationFrames += animating ? 1 : 0;
    m_record.walkerEaseFrames += m_interaction.walkerEasing() ? 1 : 0;
    m_record.cursorFrames += fromCursor ? 1 : 0;

    m_lastState = state;
    m_lastHovered = hovered;
    m_lastFocused = focused;
    m_lastPulled.assign(pulled.begin(), pulled.end());
}

void WorldHover::syncAnimationPhase(bool animating) {
    if (animating == m_animationOpen) {
        return;
    }
    m_animationOpen = animating;
    if (animating) {
        m_context.frameTiming().openPhase(kPhaseAnimation);
    } else {
        m_context.frameTiming().closePhase(kPhaseAnimation);
    }
}

std::span<const scene::RenderItem> WorldHover::viewItems(std::span<const scene::RenderItem> sceneItems,
                                                         const scene::Camera& camera) {
    if (m_collection == nullptr || m_room == nullptr || m_hidden.empty()) {
        return sceneItems;
    }
    m_viewItems.assign(sceneItems.begin(), sceneItems.end());
    for (const PulledItem& pulled : m_interaction.pulledItems()) {
        if (const world::CollectionItem* item = m_room->find(pulled.id)) {
            const glm::mat4 model = pulledItemTransform(*item, pulled.progress, m_room->reachOf(pulled.id));
            m_collection->appendRenderItems(pulled.id,
                                            m_inspection.itemModel(m_interaction, *m_room, *item, model, camera),
                                            m_viewItems);
        }
    }
    return m_viewItems;
}

std::optional<WorldLabelContent> WorldHover::labelFor(const scene::Camera& camera, glm::ivec2 viewSize) {
    m_record.label.clear();
    const world::PickId id = m_interaction.highlighted();
    const world::CollectionItem* item = m_room != nullptr ? m_room->find(id) : nullptr;
    if (item == nullptr || viewSize.x <= 0 || viewSize.y <= 0) {
        return std::nullopt;
    }
    scene::Camera view = camera;
    view.setAspect(static_cast<float>(viewSize.x) / static_cast<float>(viewSize.y));
    const auto anchor = labelAnchor(itemBounds(*item, m_interaction.pullOf(id), m_room->reachOf(id)),
                                    view.viewProjection(), viewSize,
                                    world_label::kTopMargin);
    if (!anchor) {
        return std::nullopt;
    }
    ++m_record.labelFrames;
    m_record.label = item->title;
    return WorldLabelContent{item->title, (item->flags & scene::instance_flags::kUnavailable) == 0, anchor->x,
                             anchor->y};
}

std::string WorldHover::focusedGame() const {
    const world::CollectionItem* item = m_room != nullptr ? m_room->find(m_interaction.focused()) : nullptr;
    return item != nullptr ? item->game.toString() : std::string();
}

void WorldHover::record(InteractionRecord& record) const {
    record = m_record;
    if (!record.bound) {
        return;
    }
    m_inspection.record(record.inspect);
    record.state = std::string(toString(m_interaction.state()));
    record.hoveredPickId = pickText(m_interaction.hovered());
    record.focusedPickId = pickText(m_interaction.focused());
    record.highlightedPickId = pickText(m_interaction.highlighted());
    if (const world::CollectionItem* item = m_room != nullptr ? m_room->find(m_interaction.focused()) : nullptr) {
        record.focusedGame = item->game.toString();
    }
    record.pulled.clear();
    for (const PulledItem& item : m_interaction.pulledItems()) {
        record.pulled.push_back(pickText(item.id));
    }
}

} // namespace rv::app
