#include <rv/app/world_edit_interaction.hpp>

#include <array>
#include <cmath>
#include <utility>

namespace rv::app {
namespace {

using input::Action;

constexpr std::array<EditMenuButton, 4> kMenuButtons{EditMenuButton::Save, EditMenuButton::Undo,
                                                     EditMenuButton::Discard, EditMenuButton::Continue};

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

[[nodiscard]] float axisOf(const input::ActionState& actions, Action action) noexcept {
    const float value = actions.axis(action);
    return std::isfinite(value) ? value : 0.0f;
}

/// The player moved the view or the feet this frame (a step's target then gives way to the reticle): `actions` as the
/// walker gets them (walkerActionsFor(WorldContext::Edit, ...)), so that an arrow key, bound to a step and to walking,
/// steps without giving its own target away the same frame.
[[nodiscard]] bool movedView(const input::ActionState& actions) noexcept {
    for (const Action action : {Action::LookX, Action::LookY, Action::LookDeltaX, Action::LookDeltaY,
                                Action::MoveForward, Action::MoveRight}) {
        if (axisOf(actions, action) != 0.0f) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string_view toString(EditMode mode) noexcept {
    switch (mode) {
    case EditMode::Preparing:
        return "preparing";
    case EditMode::Browse:
        return "browse";
    case EditMode::Holding:
        return "holding";
    case EditMode::Busy:
        return "busy";
    case EditMode::Menu:
        return "menu";
    case EditMode::Saving:
        return "saving";
    case EditMode::Moving:
        return "moving";
    }
    return "unknown";
}

std::string_view toString(EditMenuButton button) noexcept {
    switch (button) {
    case EditMenuButton::Save:
        return "save";
    case EditMenuButton::Undo:
        return "undo";
    case EditMenuButton::Discard:
        return "discard";
    case EditMenuButton::Continue:
        return "continue";
    }
    return "unknown";
}

EditInteraction::EditInteraction(InteractionTimings timings, bool reduceMotion)
    : m_timings(timings), m_reduceMotion(reduceMotion) {
    m_walker.setReduceMotion(reduceMotion);
}

void EditInteraction::begin(std::optional<catalog::GameId> held) {
    end();
    m_heldAtStart = std::move(held);
}

void EditInteraction::ready() {
    if (m_mode != EditMode::Preparing) {
        return;
    }
    m_mode = EditMode::Browse;
    if (m_heldAtStart && m_room != nullptr) {
        const world::PickId pick = m_room->pickOf(*m_heldAtStart);
        if (pick != world::kNoPick) {
            m_held = m_heldAtStart;
            m_heldFrom = pick;
            m_mode = EditMode::Holding;
        }
    }
    m_heldAtStart.reset();
}

void EditInteraction::end() {
    m_mode = EditMode::Preparing;
    m_room = nullptr;
    m_pending = 0;
    m_hover.reset();
    m_target.reset();
    m_stepped = false;
    m_held.reset();
    m_heldFrom = world::kNoPick;
    m_heldAtStart.reset();
    m_menuFocus = EditMenuButton::Save;
    if (m_walker.animating()) {
        m_walker.jumpTo(m_walker.pose());
    }
    m_walkerPlaced = false;
    m_probedPick = world::kNoPick;
    m_fixture.reset();
    m_probed.reset();
    m_verdict.reset();
    m_nudged = false;
}

void EditInteraction::setReduceMotion(bool on) {
    m_reduceMotion = on;
    if (on && m_walker.animating()) {
        m_walkerPlaced = true;
    }
    m_walker.setReduceMotion(on);
}

void EditInteraction::setRoom(const EditRoom* room) {
    m_room = room;
    if (m_room == nullptr) {
        m_hover.reset();
        m_target.reset();
        m_stepped = false;
        return;
    }
    // The held game where it stands now (a refused or an undone edit leaves it where it was).
    if (m_held) {
        m_heldFrom = m_room->pickOf(*m_held);
        if (m_heldFrom == world::kNoPick) {
            m_held.reset();
            if (m_mode == EditMode::Holding) {
                m_mode = EditMode::Browse;
            }
        }
    }
    // The fixture held where it stands now (a drop being shown keeps it until applied()).
    if (m_fixture && m_room->fixture(m_fixture->id) == nullptr) {
        m_fixture.reset();
        if (m_mode == EditMode::Moving) {
            m_mode = EditMode::Browse;
        }
    }
    // The target follows its slot: a pick id is the slot's, so a move leaves it on the slot the game went to.
    if (m_target) {
        m_target = m_target->fixture ? m_room->fixtureTarget(*m_target->fixture) : m_room->target(m_target->pick);
        if (!m_target) {
            m_stepped = false;
        }
    }
    if (m_hover.hovered() != world::kNoPick && !m_room->target(m_hover.hovered())) {
        m_hover.reset();
    }
}

bool EditInteraction::targetPinned() const {
    if (m_target && m_target->fixture) {
        return m_fixturePinned && m_fixturePinned(*m_target->fixture);
    }
    return m_target && m_target->game && m_pinned && m_pinned(*m_target->game);
}

std::optional<std::string> EditInteraction::verdict() const {
    if (!m_fixture) {
        return std::nullopt;
    }
    if (!m_fixture->moved()) {
        return std::string(); // where it stood
    }
    if (m_verdict && m_verdict->first == m_fixture->place) {
        return m_verdict->second;
    }
    return std::nullopt;
}

void EditInteraction::setVerdict(const world::EditOperation& probe, std::string refusal) {
    if (!m_fixture || probe.kind != world::EditKind::MoveFixture || probe.fixture != m_fixture->id) {
        return;
    }
    const world::FixtureTransform place{probe.xMm, probe.zMm, probe.yawCdeg.value_or(m_fixture->place.yawCdeg)};
    m_verdict = std::pair(place, std::move(refusal));
}

EditFrameResult EditInteraction::update(const EditFrame& frame) {
    const float dt = std::isfinite(frame.deltaSeconds) && frame.deltaSeconds > 0.0f ? frame.deltaSeconds : 0.0f;
    m_walkerNow = frame.walker;
    EditFrameResult result;
    bool looked = false;
    if (frame.actions != nullptr) {
        const input::ActionState actions = actionsFor(WorldContext::Edit, *frame.actions);
        looked = movedView(walkerActionsFor(WorldContext::Edit, *frame.actions));
        const EditMode before = m_mode;
        if (actions.pressed(Action::Back)) {
            back(frame.back, result);
        }
        if (m_mode == before && !result.shellBack) {
            switch (m_mode) {
            case EditMode::Browse:
                readBrowse(actions, frame, result);
                if (m_mode == EditMode::Browse && !result.apply) {
                    step(actions, frame);
                }
                break;
            case EditMode::Holding:
                readHolding(actions, result);
                if (m_mode == EditMode::Holding && !result.apply) {
                    step(actions, frame);
                }
                break;
            case EditMode::Menu:
                readMenu(actions, result);
                break;
            case EditMode::Moving:
                readMoving(actions, frame, looked, result);
                break;
            case EditMode::Preparing:
            case EditMode::Busy:
            case EditMode::Saving:
                break;
            }
        }
    }
    if (m_mode == EditMode::Browse || m_mode == EditMode::Holding) {
        track(frame, looked);
    }
    // I4b step 4: where a held game would go is judged ahead (a probe), so that its drop there shows what the probe
    // made instead of applying the operation again.
    if (m_mode == EditMode::Holding && m_held && m_target && m_target->pick != m_heldFrom &&
        m_target->pick != m_probedPick && !result.apply) {
        result.probe = m_target->game ? world::EditOperation::swap(*m_held, *m_target->game)
                                      : world::EditOperation::move(*m_held, m_target->slot);
        m_probedPick = m_target->pick;
    } else if (m_mode != EditMode::Holding && m_mode != EditMode::Busy) {
        m_probedPick = world::kNoPick;
    }
    const bool easing = m_walker.animating();
    m_walker.advance(dt);
    if (easing || m_walkerPlaced) {
        result.walker = m_walker.pose();
    }
    m_walkerPlaced = false;
    return result;
}

void EditInteraction::back(const BackState& shell, EditFrameResult& result) {
    // The editor's own level whatever the mode (it never lets Back quit while editing).
    const WorldBackDecision decision = worldBackAction(shell, WorldBack::LeaveFocus);
    if (decision.level == WorldBack::Shell) {
        result.shellBack = decision.shell;
        return;
    }
    switch (m_mode) {
    case EditMode::Holding:
        // Put back: it never left its slot in the draft.
        m_held.reset();
        m_heldFrom = world::kNoPick;
        m_mode = EditMode::Browse;
        return;
    case EditMode::Moving:
        // Put back where it stood: it never left in the draft.
        m_fixture.reset();
        m_mode = EditMode::Browse;
        return;
    case EditMode::Menu:
        m_mode = EditMode::Browse; // Continua
        return;
    case EditMode::Browse:
        if (m_pending > 0) {
            openMenu();
        } else {
            result.exit = true;
        }
        return;
    case EditMode::Preparing:
        result.exit = true;
        return;
    case EditMode::Busy:
    case EditMode::Saving:
        return;
    }
}

void EditInteraction::openMenu() {
    m_held.reset();
    m_heldFrom = world::kNoPick;
    m_fixture.reset();
    m_menuFocus = EditMenuButton::Save;
    m_mode = EditMode::Menu;
}

void EditInteraction::readBrowse(const input::ActionState& actions, const EditFrame& frame, EditFrameResult& result) {
    if (actions.pressed(Action::Confirm)) {
        openMenu();
        return;
    }
    if (m_target && m_target->fixture) {
        if (pickUpFixture(actions, frame, result)) {
            return;
        }
        if (actions.pressed(Action::EditPin)) {
            result.apply = targetPinned() ? world::EditOperation::unpinFixture(*m_target->fixture)
                                          : world::EditOperation::pinFixture(*m_target->fixture);
            m_mode = EditMode::Busy;
        }
        return;
    }
    if (!m_target || !m_target->game) {
        return;
    }
    if (actions.pressed(Action::Interact)) {
        m_held = m_target->game;
        m_heldFrom = m_target->pick;
        m_mode = EditMode::Holding;
        return;
    }
    if (actions.pressed(Action::EditPin)) {
        result.apply = targetPinned() ? world::EditOperation::unpin(*m_target->game)
                                      : world::EditOperation::pin(*m_target->game);
        m_mode = EditMode::Busy;
    }
}

bool EditInteraction::pickUpFixture(const input::ActionState& actions, const EditFrame& frame,
                                    EditFrameResult& result) {
    if (!actions.pressed(Action::Interact) || m_room == nullptr || !m_target || !m_target->fixture) {
        return false;
    }
    const world::EditableFixture* fixture = m_room->fixture(*m_target->fixture);
    if (fixture == nullptr) {
        return false;
    }
    if (!fixture->refusal.empty()) {
        // It cannot move (a room kept as an older template made it): the bar names why, nothing held.
        result.refused = fixture->refusal;
        return true;
    }
    HeldFixture held;
    held.id = fixture->id;
    held.role = fixture->role;
    held.hung = fixture->hung;
    held.from = fixture->at;
    held.place = fixture->at;
    held.yawCdeg = fixture->at.yawCdeg;
    // Held where the reticle took it: its pivot's offset from the floor point under the reticle (a hung decoration's
    // from the wall point), so that it does not jump to the reticle.
    if (const std::optional<world::FixtureTransform>& aim = held.hung ? frame.wallAim : frame.floorAim) {
        held.offsetXMm = fixture->at.xMm - aim->xMm;
        held.offsetZMm = fixture->at.zMm - aim->zMm;
    }
    m_fixture = held;
    m_probed = held.place;
    m_verdict = std::pair(held.place, std::string());
    m_nudged = false;
    m_target.reset();
    m_hover.reset();
    m_stepped = false;
    m_mode = EditMode::Moving;
    return true;
}

void EditInteraction::placeFixture(const world::FixtureTransform& wanted) {
    if (!m_fixture) {
        return;
    }
    if (!m_placer) {
        m_fixture->place = wanted;
        return;
    }
    if (const std::optional<world::FixtureTransform> snapped = m_placer(m_fixture->id, wanted)) {
        m_fixture->place = *snapped;
    }
}

void EditInteraction::readMoving(const input::ActionState& actions, const EditFrame& frame, bool looked,
                                 EditFrameResult& result) {
    if (!m_fixture) {
        m_mode = EditMode::Browse;
        return;
    }
    if (actions.pressed(Action::Confirm)) {
        openMenu();
        return;
    }
    // Dropped where it shows: an operation, or put back where it stood.
    if (actions.pressed(Action::Interact)) {
        if (!m_fixture->moved()) {
            m_fixture.reset();
            m_mode = EditMode::Browse;
            return;
        }
        const world::FixtureTransform& place = m_fixture->place;
        result.apply = world::EditOperation::moveFixture(m_fixture->id, place.xMm, place.zMm, place.yawCdeg);
        m_mode = EditMode::Busy;
        return;
    }
    HeldFixture& held = *m_fixture;
    // Turned: a quarter turn (decoration by 15 degrees) about its pivot; a hung decoration's wall turns it.
    if (!held.hung) {
        const std::int32_t step =
            held.role == world::FixtureRole::Decoration ? world::kDecorationTurnCdeg : world::kFixtureTurnCdeg;
        const int turns =
            (actions.pressed(Action::EditRotateLeft) ? 1 : 0) - (actions.pressed(Action::EditRotateRight) ? 1 : 0);
        if (turns != 0) {
            held.yawCdeg = ((held.place.yawCdeg + turns * step) % 36000 + 36000) % 36000;
            placeFixture(world::FixtureTransform{held.place.xMm, held.place.zMm, held.yawCdeg});
        }
    }
    // Nudged a grid step along the room's axis nearest the view's (forward, right); a hung decoration along its wall.
    if (const std::optional<world::StepDirection> direction = stepPressed(actions)) {
        const glm::vec2 view = frame.roomForward;
        const bool alongX = std::abs(view.x) > std::abs(view.y);
        const glm::ivec2 forward =
            alongX ? glm::ivec2(view.x > 0.0f ? 1 : -1, 0) : glm::ivec2(0, view.y > 0.0f ? 1 : -1);
        const glm::ivec2 right(-forward.y, forward.x);
        glm::ivec2 step(0);
        switch (*direction) {
        case world::StepDirection::Up:
            step = forward;
            break;
        case world::StepDirection::Down:
            step = -forward;
            break;
        case world::StepDirection::Right:
            step = right;
            break;
        case world::StepDirection::Left:
            step = -right;
            break;
        }
        if (held.hung) {
            // Along its wall only: the north and south walls run along x, the west and east along z.
            const bool wallAlongX = held.place.yawCdeg % 18000 == 0;
            step = wallAlongX ? glm::ivec2(step.x, 0) : glm::ivec2(0, step.y);
        }
        if (step != glm::ivec2(0)) {
            placeFixture(world::FixtureTransform{held.place.xMm + step.x * world::kEditGridMm,
                                                 held.place.zMm + step.y * world::kEditGridMm, held.yawCdeg});
            m_nudged = true;
        }
    } else if (looked) {
        m_nudged = false;
    }
    // Following the reticle: the floor point under it (kept where it was picked up from), a hung decoration's wall.
    if (!m_nudged) {
        if (held.hung && frame.wallAim) {
            placeFixture(world::FixtureTransform{frame.wallAim->xMm + held.offsetXMm,
                                                 frame.wallAim->zMm + held.offsetZMm, held.place.yawCdeg});
        } else if (!held.hung && frame.floorAim) {
            placeFixture(world::FixtureTransform{frame.floorAim->xMm + held.offsetXMm,
                                                 frame.floorAim->zMm + held.offsetZMm, held.yawCdeg});
        }
    }
    // A new place is judged (the verdict comes back with setVerdict()).
    if (held.moved() && m_probed != held.place) {
        m_probed = held.place;
        result.probe = world::EditOperation::moveFixture(held.id, held.place.xMm, held.place.zMm, held.place.yawCdeg);
    } else if (!held.moved()) {
        m_probed = held.place;
    }
}

void EditInteraction::readHolding(const input::ActionState& actions, EditFrameResult& result) {
    if (actions.pressed(Action::Confirm)) {
        openMenu();
        return;
    }
    if (!actions.pressed(Action::Interact) || !m_target || !m_held) {
        return;
    }
    if (m_target->pick == m_heldFrom) {
        // Its own slot: put back.
        m_held.reset();
        m_heldFrom = world::kNoPick;
        m_mode = EditMode::Browse;
        return;
    }
    if (m_target->game) {
        result.apply = world::EditOperation::swap(*m_held, *m_target->game);
    } else {
        result.apply = world::EditOperation::move(*m_held, m_target->slot);
    }
    m_mode = EditMode::Busy;
}

void EditInteraction::readMenu(const input::ActionState& actions, EditFrameResult& result) {
    std::size_t index = 0;
    while (index < kMenuButtons.size() && kMenuButtons[index] != m_menuFocus) {
        ++index;
    }
    if (actions.pressed(Action::UiUp) && index > 0) {
        m_menuFocus = kMenuButtons[index - 1];
        return;
    }
    if (actions.pressed(Action::UiDown) && index + 1 < kMenuButtons.size()) {
        m_menuFocus = kMenuButtons[index + 1];
        return;
    }
    // The pad's Start (confirm alone) closes the menu as it opened it: Continua. Enter (confirm and ui_activate at once),
    // interact and ui_activate press the focused button.
    if (actions.pressed(Action::Confirm) && !actions.pressed(Action::UiActivate)) {
        m_mode = EditMode::Browse;
        return;
    }
    if (!actions.pressed(Action::Interact) && !actions.pressed(Action::UiActivate)) {
        return;
    }
    switch (m_menuFocus) {
    case EditMenuButton::Save:
        if (m_pending == 0) {
            result.exit = true;
            return;
        }
        result.save = true;
        m_mode = EditMode::Saving;
        return;
    case EditMenuButton::Undo:
        if (m_pending > 0) {
            result.undo = true;
            m_mode = EditMode::Busy;
        }
        return;
    case EditMenuButton::Discard:
        if (m_pending == 0) {
            result.exit = true;
        } else {
            result.discard = true;
            m_mode = EditMode::Saving;
        }
        return;
    case EditMenuButton::Continue:
        m_mode = EditMode::Browse;
        return;
    }
}

void EditInteraction::step(const input::ActionState& actions, const EditFrame& frame) {
    const std::optional<world::StepDirection> direction = stepPressed(actions);
    if (!direction || m_room == nullptr || !frame.inRoom) {
        return;
    }
    world::PickId from = m_target ? m_target->pick : m_heldFrom;
    if (from == world::kNoPick) {
        return;
    }
    const std::optional<world::PickId> next = m_room->step(from, *direction);
    if (!next) {
        return;
    }
    retarget(*next);
    m_stepped = true;
    if (const std::optional<scene::WalkerPose> pose = m_room->focusPose(*next, frame.viewer)) {
        if (!m_walker.animating()) {
            m_walker.jumpTo(m_walkerNow);
        }
        m_walker.animateTo(*pose, m_timings.focusEase);
        if (!m_walker.animating()) {
            m_walkerPlaced = true;
        }
    }
}

void EditInteraction::retarget(world::PickId pick) {
    m_target = m_room != nullptr ? m_room->target(pick) : std::nullopt;
    m_hover.force(m_target ? pick : world::kNoPick);
}

void EditInteraction::track(const EditFrame& frame, bool looked) {
    if (m_room == nullptr) {
        return;
    }
    if (m_stepped && (looked || m_walker.animating())) {
        // A step's target holds while its ease runs; the player's own look or walk gives the reticle back.
        if (!looked) {
            return;
        }
        m_stepped = false;
    }
    if (m_stepped || !frame.hover) {
        return;
    }
    const world::PickId before = m_hover.hovered();
    const world::PickId after = m_hover.update(*frame.hover);
    // I4b step 4: where no game or slot is, a movable fixture (browsing only: a held game drops into slots).
    if (after == world::kNoPick && m_mode == EditMode::Browse && frame.fixture) {
        if (!m_target || m_target->fixture != frame.fixture) {
            m_target = m_room->fixtureTarget(*frame.fixture);
        }
        return;
    }
    if (after == before && m_target && !m_target->fixture && m_target->pick == after) {
        return;
    }
    m_target = after != world::kNoPick ? m_room->target(after) : std::nullopt;
    if (after != world::kNoPick && !m_target) {
        m_hover.reset();
    }
}

void EditInteraction::applied(bool accepted) {
    if (m_mode != EditMode::Busy) {
        return;
    }
    if (m_fixture) {
        // A fixture dropped: put down, or still in the hand where the refusal found it (the bar naming the rule).
        if (accepted) {
            m_fixture.reset();
            m_mode = EditMode::Browse;
        } else {
            m_mode = EditMode::Moving;
        }
        return;
    }
    if (accepted || !m_held) {
        m_held.reset();
        m_heldFrom = world::kNoPick;
        m_mode = EditMode::Browse;
        return;
    }
    m_mode = EditMode::Holding; // refused: still in the hand, the bar names the rule
}

void EditInteraction::undone() {
    if (m_mode == EditMode::Busy || m_mode == EditMode::Menu) {
        m_mode = EditMode::Browse;
    }
}

void EditInteraction::saved(bool written) {
    if (m_mode != EditMode::Saving) {
        return;
    }
    m_mode = written ? EditMode::Preparing : EditMode::Browse;
}

} // namespace rv::app
