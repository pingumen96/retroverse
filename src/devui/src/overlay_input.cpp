#include <rv/devui/overlay_input.hpp>

namespace rv::devui {

EventRoute routeOverlayEvent(InputEventKind kind, const OverlayInputState& state) noexcept {
    if (!state.visible) {
        return {false, true};
    }
    switch (kind) {
    case InputEventKind::MouseMotion:
    case InputEventKind::MouseButtonDown:
    case InputEventKind::MouseWheel:
        return state.mouseCaptured ? EventRoute{false, true} : EventRoute{true, !state.wantsMouse};
    case InputEventKind::MouseButtonUp:
        return {!state.mouseCaptured, true};
    case InputEventKind::KeyDown:
        return {true, !state.wantsKeyboard};
    case InputEventKind::KeyUp:
        return {true, true};
    case InputEventKind::TextInput:
        return {true, false};
    case InputEventKind::Other:
        return {true, true};
    }
    return {true, true};
}

bool CaptureHandover::open(bool captured) noexcept {
    m_restore = captured;
    return captured;
}

bool CaptureHandover::close(bool captured) noexcept {
    const bool capture = m_restore && !captured;
    m_restore = false;
    return capture;
}

} // namespace rv::devui
