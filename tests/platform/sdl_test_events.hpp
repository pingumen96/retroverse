#pragma once

// Synthetic SDL events and SDL subsystem scopes for the platform tests.

#include <initializer_list>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>

namespace rv::test {

inline SDL_Event keyEvent(SDL_Scancode scancode, bool down, bool repeat = false, SDL_Keycode key = SDLK_UNKNOWN) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.key = key;
    event.key.down = down;
    event.key.repeat = repeat;
    return event;
}

inline SDL_Event mouseButtonEvent(Uint8 button, bool down, SDL_WindowID window = 0) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.windowID = window;
    event.button.button = button;
    event.button.down = down;
    event.button.clicks = 1;
    return event;
}

inline SDL_Event mouseMotionEvent(float dx, float dy) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.xrel = dx;
    event.motion.yrel = dy;
    return event;
}

inline SDL_Event mouseWheelEvent(float y, SDL_MouseWheelDirection direction = SDL_MOUSEWHEEL_NORMAL) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.y = y;
    event.wheel.direction = direction;
    return event;
}

inline SDL_Event windowEvent(SDL_EventType type, SDL_WindowID window = 0) {
    SDL_Event event{};
    event.type = type;
    event.window.windowID = window;
    return event;
}

inline SDL_Event gamepadButtonEvent(SDL_JoystickID pad, SDL_GamepadButton button, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    event.gbutton.which = pad;
    event.gbutton.button = static_cast<Uint8>(button);
    event.gbutton.down = down;
    return event;
}

/// SDL initialized with the gamepad subsystem and only the virtual joystick driver: controllers plugged into the
/// machine stay invisible, so tests see exactly the pads they attach. Background events are allowed (no window has
/// focus in a test). Quits SDL on destruction.
class VirtualGamepadScope {
public:
    VirtualGamepadScope() {
        for (const char* hint : {SDL_HINT_JOYSTICK_RAWINPUT, SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_HIDAPI,
                                 SDL_HINT_JOYSTICK_DIRECTINPUT, SDL_HINT_XINPUT_ENABLED, SDL_HINT_JOYSTICK_GAMEINPUT}) {
            SDL_SetHint(hint, "0");
        }
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        m_ready = SDL_Init(SDL_INIT_GAMEPAD);
    }
    VirtualGamepadScope(const VirtualGamepadScope&) = delete;
    VirtualGamepadScope& operator=(const VirtualGamepadScope&) = delete;
    ~VirtualGamepadScope() {
        SDL_Quit();
        SDL_ResetHints();
    }

    [[nodiscard]] bool ready() const noexcept { return m_ready; }

    /// Attaches a virtual gamepad with every standard button and axis (virtual index i = SDL_GamepadButton or
    /// SDL_GamepadAxis i); 0 on failure. Drive it through a handle from SDL_OpenJoystick().
    SDL_JoystickID attach(const char* name) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = name;
        return SDL_AttachVirtualJoystick(&desc);
    }

private:
    bool m_ready = false;
};

/// SDL video with one hidden window (no OpenGL); quits SDL on destruction.
class HiddenWindowScope {
public:
    HiddenWindowScope() {
        if (SDL_Init(SDL_INIT_VIDEO)) {
            m_window = SDL_CreateWindow("rv_platform_tests", 64, 64, SDL_WINDOW_HIDDEN);
        }
    }
    HiddenWindowScope(const HiddenWindowScope&) = delete;
    HiddenWindowScope& operator=(const HiddenWindowScope&) = delete;
    ~HiddenWindowScope() {
        if (m_window != nullptr) {
            SDL_DestroyWindow(m_window);
        }
        SDL_Quit();
    }

    [[nodiscard]] SDL_Window* window() const noexcept { return m_window; }
    [[nodiscard]] SDL_WindowID id() const noexcept { return m_window != nullptr ? SDL_GetWindowID(m_window) : 0; }

private:
    SDL_Window* m_window = nullptr;
};

} // namespace rv::test
