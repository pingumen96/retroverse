#include <rv/ui/regenerate_preview_model.hpp>

#include <algorithm>
#include <utility>

namespace rv::ui {
namespace {

constexpr std::array<RegenerateButton, 6> kRegenerateButtons{RegenerateButton::Regenerate,  RegenerateButton::Drop,
                                                             RegenerateButton::ConfirmDrop, RegenerateButton::Undo,
                                                             RegenerateButton::Back,        RegenerateButton::Cancel};

} // namespace

std::string_view toString(RegenerateKind kind) noexcept {
    switch (kind) {
    case RegenerateKind::Room:
        return regenerate_preview::kKinds[0];
    case RegenerateKind::Floor:
        return regenerate_preview::kKinds[1];
    case RegenerateKind::World:
        return regenerate_preview::kKinds[2];
    case RegenerateKind::Undo:
        return regenerate_preview::kKinds[3];
    }
    return "unknown";
}

std::string_view toString(RegenerateState state) noexcept {
    switch (state) {
    case RegenerateState::Computing:
        return regenerate_preview::kStates[0];
    case RegenerateState::Ready:
        return regenerate_preview::kStates[1];
    case RegenerateState::Confirm:
        return regenerate_preview::kStates[2];
    case RegenerateState::Committing:
        return regenerate_preview::kStates[3];
    case RegenerateState::Failed:
        return regenerate_preview::kStates[4];
    }
    return "unknown";
}

std::string_view toString(RegenerateButton button) noexcept {
    switch (button) {
    case RegenerateButton::Regenerate:
        return "regenerate";
    case RegenerateButton::Drop:
        return "drop";
    case RegenerateButton::ConfirmDrop:
        return "confirm-drop";
    case RegenerateButton::Undo:
        return "undo";
    case RegenerateButton::Back:
        return "back";
    case RegenerateButton::Cancel:
        return "cancel";
    }
    return "unknown";
}

std::optional<RegenerateButton> regenerateButtonFromName(std::string_view name) noexcept {
    for (const RegenerateButton button : kRegenerateButtons) {
        if (toString(button) == name) {
            return button;
        }
    }
    return std::nullopt;
}

std::string_view elementIdOf(RegenerateButton button) noexcept {
    switch (button) {
    case RegenerateButton::Regenerate:
        return regenerate_preview::kRegenerateId;
    case RegenerateButton::Drop:
        return regenerate_preview::kDropId;
    case RegenerateButton::ConfirmDrop:
        return regenerate_preview::kConfirmDropId;
    case RegenerateButton::Undo:
        return regenerate_preview::kUndoId;
    case RegenerateButton::Back:
        return regenerate_preview::kBackId;
    case RegenerateButton::Cancel:
        return regenerate_preview::kCancelId;
    }
    return regenerate_preview::kCancelId;
}

std::string_view toString(RegenerateCommandKind kind) noexcept {
    switch (kind) {
    case RegenerateCommandKind::Regenerate:
        return "regenerate";
    case RegenerateCommandKind::RegenerateDrop:
        return "regenerate-drop";
    case RegenerateCommandKind::Undo:
        return "undo";
    case RegenerateCommandKind::Cancel:
        return "cancel";
    }
    return "unknown";
}

void RegeneratePreviewModel::open(RegenerateKind kind, RegenerateScopeTitle scope) {
    m_open = true;
    m_kind = kind;
    m_state = RegenerateState::Computing;
    m_scope = std::move(scope);
    m_content = RegeneratePreviewContent{};
    m_failure.clear();
    m_focused = RegenerateButton::Cancel;
    changed();
}

void RegeneratePreviewModel::setContent(RegeneratePreviewContent content) {
    if (!m_open || m_state != RegenerateState::Computing) {
        return;
    }
    m_content = std::move(content);
    m_state = RegenerateState::Ready;
    if (m_kind == RegenerateKind::Undo) {
        m_focused = RegenerateButton::Undo;
    } else if (m_content.changed) {
        m_focused = RegenerateButton::Regenerate;
    } else if (m_content.overrides > 0) {
        m_focused = RegenerateButton::Drop;
    } else {
        m_focused = RegenerateButton::Cancel;
    }
    changed();
}

void RegeneratePreviewModel::setFailed(std::string failure) {
    if (!m_open) {
        return;
    }
    m_state = RegenerateState::Failed;
    m_failure = std::move(failure);
    m_focused = RegenerateButton::Cancel;
    changed();
}

void RegeneratePreviewModel::close() noexcept {
    if (!m_open) {
        return;
    }
    m_open = false;
    changed();
}

std::vector<RegenerateButton> RegeneratePreviewModel::buttons() const {
    if (!m_open) {
        return {};
    }
    switch (m_state) {
    case RegenerateState::Computing:
    case RegenerateState::Failed:
        return {RegenerateButton::Cancel};
    case RegenerateState::Ready: {
        if (m_kind == RegenerateKind::Undo) {
            return {RegenerateButton::Undo, RegenerateButton::Cancel};
        }
        std::vector<RegenerateButton> shown;
        if (m_content.changed) {
            shown.push_back(RegenerateButton::Regenerate);
        }
        if (m_content.overrides > 0) {
            shown.push_back(RegenerateButton::Drop);
        }
        shown.push_back(RegenerateButton::Cancel);
        return shown;
    }
    case RegenerateState::Confirm:
        return {RegenerateButton::ConfirmDrop, RegenerateButton::Back};
    case RegenerateState::Committing:
        return {};
    }
    return {};
}

std::string RegeneratePreviewModel::focusedElement() const {
    const std::vector<RegenerateButton> shown = buttons();
    if (std::find(shown.begin(), shown.end(), m_focused) == shown.end()) {
        return {};
    }
    return std::string(elementIdOf(m_focused));
}

void RegeneratePreviewModel::focus(RegenerateButton button) noexcept {
    if (m_focused != button) {
        m_focused = button;
        changed();
    }
}

std::optional<RegenerateCommandKind> RegeneratePreviewModel::cancel() {
    close();
    return RegenerateCommandKind::Cancel;
}

std::optional<RegenerateCommandKind> RegeneratePreviewModel::press(RegenerateButton button) {
    const std::vector<RegenerateButton> shown = buttons();
    if (std::find(shown.begin(), shown.end(), button) == shown.end()) {
        return std::nullopt;
    }
    switch (button) {
    case RegenerateButton::Regenerate:
        m_state = RegenerateState::Committing;
        changed();
        return RegenerateCommandKind::Regenerate;
    case RegenerateButton::Drop:
        // ADR-0015 §5: the second confirmation, naming how many customizations go; "Indietro" first, so that the
        // button pressed twice drops nothing.
        m_state = RegenerateState::Confirm;
        m_focused = RegenerateButton::Back;
        changed();
        return std::nullopt;
    case RegenerateButton::ConfirmDrop:
        m_state = RegenerateState::Committing;
        changed();
        return RegenerateCommandKind::RegenerateDrop;
    case RegenerateButton::Undo:
        m_state = RegenerateState::Committing;
        changed();
        return RegenerateCommandKind::Undo;
    case RegenerateButton::Back:
        m_state = RegenerateState::Ready;
        m_focused = RegenerateButton::Drop;
        changed();
        return std::nullopt;
    case RegenerateButton::Cancel:
        return cancel();
    }
    return std::nullopt;
}

std::optional<RegenerateCommandKind> RegeneratePreviewModel::apply(UiCommand command) {
    if (!m_open) {
        return std::nullopt;
    }
    switch (command) {
    case UiCommand::Up:
    case UiCommand::Left:
    case UiCommand::Down:
    case UiCommand::Right: {
        const std::vector<RegenerateButton> shown = buttons();
        if (shown.empty()) {
            return std::nullopt;
        }
        const auto at = std::find(shown.begin(), shown.end(), m_focused);
        const std::size_t index = at != shown.end() ? static_cast<std::size_t>(at - shown.begin()) : 0;
        const bool forward = command == UiCommand::Down || command == UiCommand::Right;
        const std::size_t next = forward ? std::min(index + 1, shown.size() - 1) : (index > 0 ? index - 1 : 0);
        focus(shown[next]);
        return std::nullopt;
    }
    case UiCommand::Activate:
        return press(m_focused);
    case UiCommand::Back:
        if (m_state == RegenerateState::Committing) {
            return std::nullopt;
        }
        if (m_state == RegenerateState::Confirm) {
            return press(RegenerateButton::Back);
        }
        return cancel();
    default:
        return std::nullopt;
    }
}

std::optional<RegenerateCommandKind> RegeneratePreviewModel::click(RegenerateButton button) {
    if (!m_open) {
        return std::nullopt;
    }
    const std::vector<RegenerateButton> shown = buttons();
    if (std::find(shown.begin(), shown.end(), button) == shown.end()) {
        return std::nullopt;
    }
    focus(button);
    return press(button);
}

} // namespace rv::ui
