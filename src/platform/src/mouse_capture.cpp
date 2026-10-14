#include <rv/platform/mouse_capture.hpp>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <spdlog/spdlog.h>

namespace rv::platform {

MouseCapture::MouseCapture(SDL_Window* window) noexcept
    : m_window(window), m_windowId(window != nullptr ? SDL_GetWindowID(window) : 0u),
      m_focused(window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0) {}

MouseCapture::~MouseCapture() {
    if (m_captured && m_window != nullptr) {
        SDL_SetWindowRelativeMouseMode(m_window, false);
    }
}

bool MouseCapture::handleEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (event.window.windowID == m_windowId) {
            m_focused = true;
            if (m_captureOnFocus) {
                m_captureOnFocus = false;
                apply(true, "the window gained focus");
            }
        }
        return false;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (event.window.windowID == m_windowId) {
            m_focused = false;
            apply(false, "the window lost focus");
        }
        return false;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.windowID == m_windowId && m_focused && !m_captured) {
            m_captureOnFocus = false;
            // Consumed even if SDL refused: the click was meant to grab the mouse, not to act in the scene.
            apply(true, "click into the window");
            return true;
        }
        return false;
    default:
        return false;
    }
}

void MouseCapture::captureWhenFocused() {
    if (m_focused) {
        apply(true, "startup");
    } else {
        m_captureOnFocus = true;
    }
}

bool MouseCapture::setCaptured(bool captured, std::string_view reason) {
    m_captureOnFocus = false;
    return apply(captured, reason);
}

bool MouseCapture::apply(bool captured, std::string_view reason) {
    if (captured == m_captured || m_window == nullptr) {
        return m_captured;
    }
    if (!SDL_SetWindowRelativeMouseMode(m_window, captured)) {
        spdlog::warn("Could not {} the mouse ({}): {}", captured ? "capture" : "release", reason, SDL_GetError());
        return m_captured;
    }
    m_captured = captured;
    if (captured) {
        spdlog::info("Mouse captured ({}): move it to look around, Esc releases it", reason);
    } else {
        spdlog::info("Mouse released ({})", reason);
    }
    return m_captured;
}

} // namespace rv::platform
