#include "virtual_pad.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <SDL3/SDL_error.h>

namespace rv::spike_ui {

Expected<VirtualPad> VirtualPad::attach(const char* name) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = name;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (id == 0) {
        return makeError(ErrorCode::PlatformError, std::string("SDL_AttachVirtualJoystick failed: ") + SDL_GetError());
    }
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    if (joystick == nullptr) {
        std::string error = SDL_GetError();
        SDL_DetachVirtualJoystick(id);
        return makeError(ErrorCode::PlatformError, "SDL_OpenJoystick of the virtual pad failed: " + error);
    }
    return VirtualPad(id, joystick);
}

VirtualPad::VirtualPad(VirtualPad&& other) noexcept
    : m_id(std::exchange(other.m_id, 0)), m_joystick(std::exchange(other.m_joystick, nullptr)) {}

VirtualPad& VirtualPad::operator=(VirtualPad&& other) noexcept {
    if (this != &other) {
        release();
        m_id = std::exchange(other.m_id, 0);
        m_joystick = std::exchange(other.m_joystick, nullptr);
    }
    return *this;
}

VirtualPad::~VirtualPad() {
    release();
}

void VirtualPad::release() noexcept {
    if (m_joystick != nullptr) {
        SDL_CloseJoystick(m_joystick);
        m_joystick = nullptr;
    }
    if (m_id != 0) {
        SDL_DetachVirtualJoystick(m_id);
        m_id = 0;
    }
}

bool VirtualPad::button(SDL_GamepadButton button, bool down) noexcept {
    return m_joystick != nullptr && SDL_SetJoystickVirtualButton(m_joystick, button, down);
}

bool VirtualPad::axis(SDL_GamepadAxis axis, float value) noexcept {
    if (m_joystick == nullptr) {
        return false;
    }
    float clamped = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
    if (axis == SDL_GAMEPAD_AXIS_LEFTY || axis == SDL_GAMEPAD_AXIS_RIGHTY) {
        clamped = -clamped; // rv::input sticks are +Y up, SDL's +Y down
    }
    const auto raw = static_cast<Sint16>(std::lround(clamped >= 0.0f ? clamped * SDL_JOYSTICK_AXIS_MAX
                                                                      : -clamped * SDL_JOYSTICK_AXIS_MIN));
    return SDL_SetJoystickVirtualAxis(m_joystick, axis, raw);
}

} // namespace rv::spike_ui
