#pragma once

#include <cstdint>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <rv/core/expected.hpp>

namespace rv::spike_ui {

/// A gamepad that exists only in software (SDL's virtual joystick driver, every standard button and axis), driven by
/// the scripted scenario: its presses travel the real path, SDL gamepad events -> platform::GamepadManager (hot-plug)
/// -> platform::SdlInputCollector -> the action map (input::InputSystem) -> ui::NavInput. State set during a frame
/// reaches the application with the next
/// frame's event poll. Move-only RAII: detached on destruction. Main thread only; SDL must be initialized with the
/// gamepad subsystem (app::AppHost does) for its whole lifetime.
class VirtualPad {
public:
    /// Attaches the pad and opens a handle to drive it. Errors: PlatformError with SDL's message.
    [[nodiscard]] static Expected<VirtualPad> attach(const char* name);

    VirtualPad(VirtualPad&& other) noexcept;
    VirtualPad& operator=(VirtualPad&& other) noexcept;
    VirtualPad(const VirtualPad&) = delete;
    VirtualPad& operator=(const VirtualPad&) = delete;
    ~VirtualPad();

    [[nodiscard]] SDL_JoystickID id() const noexcept { return m_id; }
    /// Presses or releases a button; false (logged once by the caller) when SDL refuses.
    bool button(SDL_GamepadButton button, bool down) noexcept;
    /// Moves an axis to `value` in [-1, 1] in rv::input conventions (sticks +Y up), converted to SDL's range and
    /// direction (SDL's Y axes point down).
    bool axis(SDL_GamepadAxis axis, float value) noexcept;

private:
    VirtualPad(SDL_JoystickID id, SDL_Joystick* joystick) noexcept : m_id(id), m_joystick(joystick) {}
    void release() noexcept;

    SDL_JoystickID m_id = 0;
    SDL_Joystick* m_joystick = nullptr;
};

} // namespace rv::spike_ui
