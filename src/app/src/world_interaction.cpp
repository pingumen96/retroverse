#include <rv/app/world_interaction.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>

#include <rv/app/input_context.hpp>
#include <rv/core/math.hpp>

namespace rv::app {

namespace {

using input::Action;

[[nodiscard]] float finiteOrZero(float value) noexcept {
    return std::isfinite(value) ? value : 0.0f;
}

constexpr Action kLookActions[] = {Action::LookX, Action::LookY, Action::LookDeltaX, Action::LookDeltaY};

/// How long a look keeps the right to move the focus in Focus: the hover needs world::kHoverChangeFrames frames of the
/// ray on a new item after the look moved it (the ray of a frame is cast before its look applies).
constexpr int kLookMemoryFrames = world::kHoverChangeFrames;

[[nodiscard]] bool isArrow(Action action) noexcept {
    return action == Action::UiUp || action == Action::UiDown || action == Action::UiLeft || action == Action::UiRight;
}

/// The D-pad step a frame asks for: the first of up, down, left, right pressed.
[[nodiscard]] std::optional<world::StepDirection> stepPressed(const input::ActionState& actions) noexcept {
    if (actions.pressed(Action::UiUp)) {
        return world::StepDirection::Up;
    }
    if (actions.pressed(Action::UiDown)) {
        return world::StepDirection::Down;
    }
    if (actions.pressed(Action::UiLeft)) {
        return world::StepDirection::Left;
    }
    if (actions.pressed(Action::UiRight)) {
        return world::StepDirection::Right;
    }
    return std::nullopt;
}

} // namespace

std::string_view toString(WorldState state) noexcept {
    switch (state) {
    case WorldState::FreeRoam:
        return "free roam";
    case WorldState::Focus:
        return "focus";
    case WorldState::Inspect:
        return "inspect";
    case WorldState::Launching:
        return "launching";
    case WorldState::Returning:
        return "returning";
    }
    return "unknown";
}

std::string_view toString(WorldContext context) noexcept {
    switch (context) {
    case WorldContext::FreeRoam:
        return "free roam";
    case WorldContext::Focus:
        return "focus";
    case WorldContext::Inspect:
        return "inspect";
    case WorldContext::Suspended:
        return "suspended";
    }
    return "unknown";
}

std::string_view toString(WorldBack level) noexcept {
    switch (level) {
    case WorldBack::Shell:
        return "shell";
    case WorldBack::LeaveInspect:
        return "leave inspect";
    case WorldBack::SkipLowering:
        return "skip lowering";
    case WorldBack::LeaveFocus:
        return "leave focus";
    }
    return "unknown";
}

std::string_view toString(InteractionNotice notice) noexcept {
    switch (notice) {
    case InteractionNotice::None:
        return "none";
    case InteractionNotice::ItemUnavailable:
        return "item unavailable";
    case InteractionNotice::LaunchOutcome:
        return "launch outcome";
    }
    return "unknown";
}

std::string_view toString(CardAction action) noexcept {
    switch (action) {
    case CardAction::Launch:
        return "launch";
    case CardAction::Media:
        return "media";
    case CardAction::Info:
        return "info";
    case CardAction::Back:
        return "back";
    }
    return "unknown";
}

bool readsAction(WorldContext context, Action action) noexcept {
    if (isGlobalAction(action)) {
        return true;
    }
    switch (context) {
    case WorldContext::FreeRoam:
        return readsAction(InputContext::World, action);
    case WorldContext::Focus:
        return input::isLookAction(action) || isArrow(action) || action == Action::Interact ||
               action == Action::UiActivate || action == Action::Back || action == Action::QuickCatalog;
    case WorldContext::Inspect:
        return input::isLookAction(action) || isArrow(action) || action == Action::UiPageUp ||
               action == Action::UiPageDown || action == Action::UiActivate || action == Action::Confirm ||
               action == Action::UiMedia || action == Action::UiInfo || action == Action::UiBack ||
               action == Action::Interact;
    case WorldContext::Suspended:
        return false;
    }
    return false;
}

input::ActionState actionsFor(WorldContext context, const input::ActionState& actions) noexcept {
    input::ActionState read = actions;
    for (std::size_t index = 0; index < input::kActionCount; ++index) {
        const auto action = static_cast<Action>(index);
        if (!readsAction(context, action)) {
            read.setButton(action, false, false, false);
            read.setAxis(action, 0.0f);
        }
    }
    return read;
}

input::ActionState walkerActionsFor(WorldContext context, const input::ActionState& actions) noexcept {
    switch (context) {
    case WorldContext::FreeRoam:
        return actionsFor(WorldContext::FreeRoam, actions);
    case WorldContext::Focus: {
        input::ActionState read = actions;
        for (std::size_t index = 0; index < input::kActionCount; ++index) {
            const auto action = static_cast<Action>(index);
            if (!input::isLookAction(action)) {
                read.setButton(action, false, false, false);
                read.setAxis(action, 0.0f);
            }
        }
        return read;
    }
    case WorldContext::Inspect:
    case WorldContext::Suspended:
        break;
    }
    input::ActionState none = actions;
    none.clear();
    return none;
}

WorldBackDecision worldBackAction(const BackState& shell, WorldBack world) noexcept {
    const BackAction action = backAction(shell);
    switch (action) {
    case BackAction::Ignore:
    case BackAction::ClosePopup:
    case BackAction::PanelBack:
    case BackAction::CatalogBack:
        return WorldBackDecision{WorldBack::Shell, action};
    case BackAction::ReleaseMouse:
    case BackAction::CloseOverlay:
    case BackAction::Quit:
        break;
    }
    return WorldBackDecision{world, action};
}

WorldInteraction::WorldInteraction(InteractionSettings settings)
    : m_settings(settings), m_lift(0.0f, scene::Easing::EaseInOut) {
    InspectSettings& inspect = m_settings.inspect;
    inspect.minDistance = std::max(finiteOrZero(inspect.minDistance), 0.0f);
    inspect.maxDistance = std::max(finiteOrZero(inspect.maxDistance), inspect.minDistance);
    inspect.distance = std::clamp(finiteOrZero(inspect.distance), inspect.minDistance, inspect.maxDistance);
    inspect.pitchLimitDeg = std::clamp(finiteOrZero(inspect.pitchLimitDeg), 0.0f, 89.0f);
    m_inspectDistance = inspect.distance;
    m_walker.setReduceMotion(m_settings.reduceMotion);
    m_lift.setReduceMotion(m_settings.reduceMotion);
}

void WorldInteraction::setRoom(const InteractionRoom* room) {
    m_room = room;
    roomChanged();
}

std::optional<RoomItem> WorldInteraction::roomItem(world::PickId id) const {
    if (m_room == nullptr || id == world::kNoPick) {
        return std::nullopt;
    }
    return m_room->item(id);
}

void WorldInteraction::roomChanged() {
    if (m_hover.hovered() != world::kNoPick && !roomItem(m_hover.hovered())) {
        m_hover.reset();
        m_dwell = 0.0f;
        m_dwellPulls = true;
    }
    std::erase_if(m_pulls, [&](const Pull& pull) { return !roomItem(pull.id); });
    if ((m_state == WorldState::Focus || m_state == WorldState::Inspect) && !roomItem(m_focused)) {
        toFreeRoam(InteractionNotice::ItemUnavailable);
    }
    advancePulls(0.0f);
}

WorldBack WorldInteraction::backLevel() const noexcept {
    switch (m_state) {
    case WorldState::Inspect:
        return WorldBack::LeaveInspect;
    case WorldState::Focus:
        // In Focus the lift moves only while the item is lowered back from Inspect.
        return m_lift.animating() ? WorldBack::SkipLowering : WorldBack::LeaveFocus;
    case WorldState::FreeRoam:
    case WorldState::Launching:
    case WorldState::Returning:
        break;
    }
    return WorldBack::Shell;
}

world::PickId WorldInteraction::highlighted() const noexcept {
    switch (m_state) {
    case WorldState::FreeRoam:
        return m_hover.hovered();
    case WorldState::Focus:
        return m_focused;
    case WorldState::Inspect:
    case WorldState::Launching:
    case WorldState::Returning:
        break;
    }
    return world::kNoPick;
}

float WorldInteraction::pullOf(world::PickId id) const noexcept {
    for (const PulledItem& pulled : m_pulledView) {
        if (pulled.id == id) {
            return pulled.progress;
        }
    }
    return 0.0f;
}

InspectView WorldInteraction::inspectView() const noexcept {
    return InspectView{m_lift.pose(), m_inspectYaw, m_inspectPitch, m_inspectDistance};
}

void WorldInteraction::setReduceMotion(bool on) {
    m_settings.reduceMotion = on;
    if (on && m_walker.animating()) {
        m_walkerPlaced = true;
    }
    m_walker.setReduceMotion(on);
    m_lift.setReduceMotion(on);
    for (Pull& pull : m_pulls) {
        pull.progress.setReduceMotion(on);
    }
    advancePulls(0.0f);
}

InteractionResult WorldInteraction::update(const InteractionFrame& frame) {
    const float dt = std::max(finiteOrZero(frame.deltaSeconds), 0.0f);
    m_walkerNow = frame.walker;
    InteractionResult result;

    if (frame.actions != nullptr) {
        const WorldContext context = this->context();
        const input::ActionState actions = actionsFor(context, *frame.actions);
        // Indietro on the card (I3 step 14a) is ui_back.
        const bool backPressed = context == WorldContext::Inspect
                                     ? actions.pressed(Action::UiBack) || frame.card == CardAction::Back
                                     : actions.pressed(Action::Back);
        const WorldState before = m_state;
        if (backPressed) {
            back(frame.back, result);
        }
        // A Back the world took ends the frame's reading: the state it left behind reads its actions next frame.
        if (m_state == before) {
            switch (m_state) {
            case WorldState::FreeRoam:
                readFreeRoam(actions, result);
                break;
            case WorldState::Focus:
                readFocus(actions, result);
                break;
            case WorldState::Inspect:
                readInspect(actions, dt, finiteOrZero(frame.wheelNotches), frame.card, result);
                break;
            case WorldState::Launching:
            case WorldState::Returning:
                break;
            }
        }
        // Frames since the player last looked: in Focus only a pick the eyes moved moves the focus.
        const bool looked = std::any_of(std::begin(kLookActions), std::end(kLookActions), [&](Action action) {
            return finiteOrZero(actions.axis(action)) != 0.0f;
        });
        m_framesSinceLook = looked ? 0 : std::min(m_framesSinceLook + 1, kLookMemoryFrames + 1);
        if (frame.hover && (m_state == WorldState::FreeRoam || m_state == WorldState::Focus)) {
            track(*frame.hover);
        }
    }
    if (m_state == WorldState::Returning) {
        finishReturn();
    }

    const bool easing = m_walker.animating();
    m_walker.advance(dt);
    if (easing || m_walkerPlaced) {
        result.walker = m_walker.pose();
    }
    m_walkerPlaced = false;
    m_lift.advance(dt);

    // The hover's dwell: the item it rests on is pulled out once the dwell has passed (FreeRoam; in Focus the focused
    // item is the pulled one).
    if (m_state == WorldState::FreeRoam && m_hover.hovered() != world::kNoPick && m_dwellPulls) {
        m_dwell += dt;
        if (m_dwell >= m_settings.timings.hoverDwell) {
            pull(m_hover.hovered());
            m_dwellPulls = false;
        }
    }
    advancePulls(dt);
    return result;
}

void WorldInteraction::back(const BackState& shell, InteractionResult& result) {
    const WorldBackDecision decision = worldBackAction(shell, backLevel());
    switch (decision.level) {
    case WorldBack::Shell:
        result.shellBack = decision.shell;
        return;
    case WorldBack::LeaveInspect:
        leaveInspect();
        return;
    case WorldBack::SkipLowering:
        m_lift.finish();
        return;
    case WorldBack::LeaveFocus:
        leaveFocus();
        return;
    }
}

void WorldInteraction::readFreeRoam(const input::ActionState& actions, InteractionResult& result) {
    if (actions.pressed(Action::QuickCatalog)) {
        result.openCatalog = true;
    }
    if (!actions.pressed(Action::Interact)) {
        return;
    }
    const world::PickId target = m_hover.hovered();
    if (target == world::kNoPick) {
        return;
    }
    const auto item = roomItem(target);
    if (!item) {
        m_hover.reset();
        m_notice = InteractionNotice::ItemUnavailable;
        return;
    }
    if (item->faceOut) {
        enterInspect(target, *item);
    } else {
        enterFocus(target, *item, m_settings.timings.focusEase);
    }
}

void WorldInteraction::readFocus(const input::ActionState& actions, InteractionResult& result) {
    if (actions.pressed(Action::QuickCatalog)) {
        result.openCatalog = true;
    }
    if (actions.pressed(Action::Interact) || actions.pressed(Action::UiActivate)) {
        const Pull* pulled = findPull(m_focused);
        const bool pulling = pulled != nullptr && pulled->progress.animating() && pulled->progress.target() > 0.5f;
        if (m_interactSkips && (m_walker.animating() || pulling)) {
            // The button that started the ease into Focus skips it; the next press inspects. After a D-pad step the
            // press inspects at once.
            skipWalkerAndPull();
            m_interactSkips = false;
            return;
        }
        if (const auto item = roomItem(m_focused)) {
            enterInspect(m_focused, *item);
        } else {
            toFreeRoam(InteractionNotice::ItemUnavailable);
        }
        return;
    }
    if (const auto direction = stepPressed(actions); direction && m_room != nullptr) {
        if (const auto next = m_room->step(m_focused, *direction)) {
            if (const auto item = roomItem(*next)) {
                moveFocus(*next, *item, true);
            }
        }
    }
}

void WorldInteraction::readInspect(const input::ActionState& actions, float dt, float wheelNotches,
                                   std::optional<CardAction> card, InteractionResult& result) {
    const InspectSettings& inspect = m_settings.inspect;
    // Rotation about the item's center: positive yaw turns its front to the viewer's right, positive pitch tilts it up.
    const float stickRate = core::degreesToRadians(inspect.stickDegreesPerSecond) * dt;
    m_inspectYaw += finiteOrZero(actions.axis(Action::LookDeltaX)) * inspect.mouseRadiansPerPixel +
                    std::clamp(finiteOrZero(actions.axis(Action::LookX)), -1.0f, 1.0f) * stickRate;
    m_inspectPitch += finiteOrZero(actions.axis(Action::LookDeltaY)) * inspect.mouseRadiansPerPixel +
                      std::clamp(finiteOrZero(actions.axis(Action::LookY)), -1.0f, 1.0f) * stickRate;
    m_inspectYaw = core::wrapAngleRadians(m_inspectYaw);
    const float pitchLimit = core::degreesToRadians(inspect.pitchLimitDeg);
    m_inspectPitch = std::clamp(m_inspectPitch, -pitchLimit, pitchLimit);
    // Zoom: the right trigger (Page Down) and the wheel away from the user bring the item closer.
    const float closer = std::clamp(finiteOrZero(actions.axis(Action::UiPageDown)), 0.0f, 1.0f) -
                         std::clamp(finiteOrZero(actions.axis(Action::UiPageUp)), 0.0f, 1.0f);
    m_inspectDistance -= closer * inspect.zoomMetersPerSecond * dt + wheelNotches * inspect.zoomMetersPerNotch;
    m_inspectDistance = std::clamp(m_inspectDistance, inspect.minDistance, inspect.maxDistance);

    const bool lifting = m_lift.animating() && m_lift.target() > 0.5f;
    if (lifting) {
        // The button that started the lift skips it; the card's actions wait for the item in the hand.
        if (actions.pressed(Action::Interact) || actions.pressed(Action::UiActivate)) {
            m_lift.finish();
            if (Pull* pulled = findPull(m_focused)) {
                pulled->progress.finish();
            }
        }
        return;
    }
    // Avvia: the card's button (Enter or the pad's south button on it, a click), or confirm alone (the pad's Start):
    // Enter is confirm and ui_activate at once, and presses the card's focused button instead (I3 step 14a).
    const bool startPressed = actions.pressed(Action::Confirm) && !actions.pressed(Action::UiActivate);
    if (card == CardAction::Launch || startPressed) {
        const auto item = roomItem(m_focused);
        if (!item) {
            toFreeRoam(InteractionNotice::ItemUnavailable);
            return;
        }
        m_launch = WorldLaunchRecord{m_focused, item->game, m_walkerNow, inspectView()};
        result.launch = WorldLaunchRequest{m_focused, item->game};
        m_state = WorldState::Launching;
        clearNotice();
        return;
    }
    if (card == CardAction::Media || actions.pressed(Action::UiMedia)) {
        result.showMedia = true;
    } else if (card == CardAction::Info || actions.pressed(Action::UiInfo)) {
        result.showInfo = true;
    }
}

void WorldInteraction::track(const world::HoverSample& sample) {
    const world::PickId before = m_hover.hovered();
    world::PickId after = m_hover.update(sample);
    if (after == before) {
        return;
    }
    if (after != world::kNoPick && !roomItem(after)) {
        // An index the room has already left behind: hover nothing rather than an item that is gone.
        m_hover.reset();
        after = world::kNoPick;
        if (after == before) {
            return;
        }
    }
    m_dwell = 0.0f;
    m_dwellPulls = true;
    if (m_state == WorldState::FreeRoam) {
        if (before != world::kNoPick) {
            pushBack(before);
        }
        return;
    }
    // Focus: looking moves the focus to another item of the same shelf section. Not a pick the walker's ease swept
    // (a D-pad step is exact), nor one the eyes did not move (a still stick never changes the focus).
    if (after != world::kNoPick && after != m_focused && !m_walker.animating() &&
        m_framesSinceLook <= kLookMemoryFrames) {
        if (const auto item = roomItem(after); item && item->section == m_focusedSection) {
            moveFocus(after, *item, false);
        }
    }
}

void WorldInteraction::finishReturn() {
    const WorldLaunchRecord record = m_launch.value_or(WorldLaunchRecord{});
    m_walker.jumpTo(record.walker);
    m_walkerPlaced = true;
    const std::optional<LaunchMessage> outcome = std::move(m_launchOutcome);
    m_launchOutcome.reset();
    const auto item = roomItem(record.item);
    if (!item) {
        toFreeRoam(InteractionNotice::ItemUnavailable);
        return;
    }
    m_state = WorldState::Inspect;
    m_focused = record.item;
    m_focusedSection = item->section;
    pull(record.item);
    if (Pull* pulled = findPull(record.item)) {
        pulled->progress.finish();
    }
    m_lift.jumpTo(1.0f);
    m_inspectYaw = record.inspect.yaw;
    m_inspectPitch = record.inspect.pitch;
    m_inspectDistance = record.inspect.distance;
    if (outcome && outcome->code != launch_outcome::kReturned) {
        m_notice = InteractionNotice::LaunchOutcome;
        m_noticeMessage = *outcome;
    } else {
        clearNotice();
    }
}

bool WorldInteraction::showInRoom(world::PickId id, const scene::WalkerPose& walker) {
    if (m_state != WorldState::FreeRoam && m_state != WorldState::Focus) {
        return false;
    }
    const auto item = roomItem(id);
    if (!item) {
        m_notice = InteractionNotice::ItemUnavailable;
        return false;
    }
    m_walkerNow = walker;
    enterFocus(id, *item, m_settings.timings.showInRoomEase);
    return true;
}

void WorldInteraction::launchEnded(const LaunchMessage& outcome) {
    if (m_state != WorldState::Launching) {
        return;
    }
    m_state = WorldState::Returning;
    m_launchOutcome = outcome;
}

void WorldInteraction::enterFocus(world::PickId id, const RoomItem& item, float easeSeconds) {
    m_state = WorldState::Focus;
    m_focused = id;
    m_focusedSection = item.section;
    pull(id);
    pushBackAllBut(id);
    easeWalkerTo(m_room->focusPose(id), easeSeconds);
    m_interactSkips = true;
    clearNotice();
}

void WorldInteraction::moveFocus(world::PickId id, const RoomItem& item, bool easeWalker) {
    if (id == m_focused) {
        return;
    }
    pushBack(m_focused);
    pull(id);
    m_focused = id;
    m_focusedSection = item.section;
    if (easeWalker) {
        easeWalkerTo(m_room->focusPose(id), m_settings.timings.focusEase);
    }
    m_interactSkips = false;
    clearNotice();
}

void WorldInteraction::enterInspect(world::PickId id, const RoomItem& item) {
    m_state = WorldState::Inspect;
    m_focused = id;
    m_focusedSection = item.section;
    pull(id);
    pushBackAllBut(id);
    m_inspectYaw = 0.0f;
    m_inspectPitch = 0.0f;
    m_inspectDistance = m_settings.inspect.distance;
    m_lift.animateTo(1.0f, m_settings.timings.lift);
    if (m_walker.animating()) {
        // Inspected during a D-pad step's ease: the walker does not move while the item is in the hand.
        m_walker.finish();
        m_walkerPlaced = true;
    }
    m_interactSkips = false;
    clearNotice();
}

void WorldInteraction::leaveInspect() {
    m_state = WorldState::Focus;
    m_lift.animateTo(0.0f, m_settings.timings.lower);
}

void WorldInteraction::leaveFocus() {
    const world::PickId item = m_focused;
    toFreeRoam(InteractionNotice::None);
    // Put back, it stays in its slot while the hover rests on it: pulled out again only after the hover left it.
    if (m_hover.hovered() == item) {
        m_dwellPulls = false;
    }
}

void WorldInteraction::toFreeRoam(InteractionNotice notice) {
    m_state = WorldState::FreeRoam;
    if (m_focused != world::kNoPick) {
        pushBack(m_focused);
    }
    m_focused = world::kNoPick;
    m_lift.jumpTo(0.0f);
    if (m_walker.animating()) {
        // Stopped where it is: the player walks on from there.
        m_walker.jumpTo(m_walker.pose());
        m_walkerPlaced = true;
    }
    m_dwell = 0.0f;
    m_dwellPulls = true;
    if (notice == InteractionNotice::None) {
        clearNotice();
    } else {
        m_notice = notice;
    }
}

void WorldInteraction::easeWalkerTo(const scene::WalkerPose& target, float seconds) {
    if (!m_walker.animating()) {
        // The player may have looked around since the last ease: start where the walker is.
        m_walker.jumpTo(m_walkerNow);
    }
    m_walker.animateTo(target, seconds);
    if (!m_walker.animating()) {
        m_walkerPlaced = true;
    }
}

void WorldInteraction::skipWalkerAndPull() {
    if (m_walker.animating()) {
        m_walker.finish();
        m_walkerPlaced = true;
    }
    if (Pull* pulled = findPull(m_focused)) {
        pulled->progress.finish();
    }
}

WorldInteraction::Pull* WorldInteraction::findPull(world::PickId id) noexcept {
    for (Pull& pulled : m_pulls) {
        if (pulled.id == id) {
            return &pulled;
        }
    }
    return nullptr;
}

void WorldInteraction::pull(world::PickId id) {
    if (id == world::kNoPick) {
        return;
    }
    Pull* pulled = findPull(id);
    if (pulled == nullptr) {
        m_pulls.push_back(Pull{id, scene::PoseAnimator<float>(0.0f, scene::Easing::EaseInOut)});
        pulled = &m_pulls.back();
        pulled->progress.setReduceMotion(m_settings.reduceMotion);
    }
    pulled->progress.animateTo(1.0f, m_settings.timings.pullOut);
}

void WorldInteraction::pushBack(world::PickId id) {
    if (Pull* pulled = findPull(id)) {
        pulled->progress.animateTo(0.0f, m_settings.timings.pushBack);
    }
}

void WorldInteraction::pushBackAllBut(world::PickId keep) {
    for (Pull& pulled : m_pulls) {
        if (pulled.id != keep) {
            pulled.progress.animateTo(0.0f, m_settings.timings.pushBack);
        }
    }
}

void WorldInteraction::advancePulls(float dt) {
    for (Pull& pulled : m_pulls) {
        pulled.progress.advance(dt);
    }
    // Back in its slot: drawn by its static instance again.
    std::erase_if(m_pulls, [](const Pull& pulled) {
        return !pulled.progress.animating() && pulled.progress.pose() <= 0.0f;
    });
    m_pulledView.clear();
    for (const Pull& pulled : m_pulls) {
        m_pulledView.push_back(PulledItem{pulled.id, pulled.progress.pose()});
    }
    std::sort(m_pulledView.begin(), m_pulledView.end(),
              [](const PulledItem& a, const PulledItem& b) { return a.id < b.id; });
}

void WorldInteraction::clearNotice() {
    m_notice = InteractionNotice::None;
    m_noticeMessage = LaunchMessage{};
}

} // namespace rv::app
