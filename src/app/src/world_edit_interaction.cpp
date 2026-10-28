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

/// The player moved the view or the feet this frame (a step's target then gives way to the reticle).
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
    // The target follows its slot: a pick id is the slot's, so a move leaves it on the slot the game went to.
    if (m_target) {
        m_target = m_room->target(m_target->pick);
        if (!m_target) {
            m_stepped = false;
        }
    }
    if (m_hover.hovered() != world::kNoPick && !m_room->target(m_hover.hovered())) {
        m_hover.reset();
    }
}

bool EditInteraction::targetPinned() const {
    return m_target && m_target->game && m_pinned && m_pinned(*m_target->game);
}

EditFrameResult EditInteraction::update(const EditFrame& frame) {
    const float dt = std::isfinite(frame.deltaSeconds) && frame.deltaSeconds > 0.0f ? frame.deltaSeconds : 0.0f;
    m_walkerNow = frame.walker;
    EditFrameResult result;
    bool looked = false;
    if (frame.actions != nullptr) {
        const input::ActionState actions = actionsFor(WorldContext::Edit, *frame.actions);
        looked = movedView(actions);
        const EditMode before = m_mode;
        if (actions.pressed(Action::Back)) {
            back(frame.back, result);
        }
        if (m_mode == before && !result.shellBack) {
            switch (m_mode) {
            case EditMode::Browse:
                readBrowse(actions, result);
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
    m_menuFocus = EditMenuButton::Save;
    m_mode = EditMode::Menu;
}

void EditInteraction::readBrowse(const input::ActionState& actions, EditFrameResult& result) {
    if (actions.pressed(Action::Confirm)) {
        openMenu();
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
    if (!actions.pressed(Action::Interact) && !actions.pressed(Action::UiActivate) &&
        !actions.pressed(Action::Confirm)) {
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
    if (!direction || m_room == nullptr) {
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
    if (after == before && m_target && m_target->pick == after) {
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
