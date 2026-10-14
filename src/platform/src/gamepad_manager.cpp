#include <rv/platform/gamepad_manager.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_stdinc.h>
#include <spdlog/spdlog.h>

#include <rv/platform/sdl_input_codes.hpp>

namespace rv::platform {
namespace {

std::string gamepadName(SDL_Gamepad* gamepad) {
    const char* name = SDL_GetGamepadName(gamepad);
    return (name != nullptr && *name != '\0') ? std::string(name) : std::string("unnamed gamepad");
}

} // namespace

GamepadManager::~GamepadManager() {
    closeAll();
}

void GamepadManager::openConnected() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids == nullptr) {
        spdlog::warn("Could not list the connected gamepads: {}", SDL_GetError());
        return;
    }
    for (int i = 0; i < count; ++i) {
        open(ids[i]);
    }
    SDL_free(ids);
}

void GamepadManager::handleEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        open(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        close(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        markUsed(event.gbutton.which);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        // Sticks and triggers alike: |stick value| is the normalized magnitude for both.
        if (std::abs(stickValueFromSdl(event.gaxis.value)) >= kActivityThreshold) {
            markUsed(event.gaxis.which);
        }
        break;
    default:
        break;
    }
}

void GamepadManager::closeAll() noexcept {
    if (m_pads.empty()) {
        return;
    }
    for (const Pad& pad : m_pads) {
        SDL_CloseGamepad(pad.handle);
    }
    try {
        spdlog::debug("Closed {} gamepad(s)", m_pads.size());
    } catch (...) {
        // Closing must not fail because of logging.
    }
    m_pads.clear();
    m_active.reset();
}

bool GamepadManager::isOpen(SDL_JoystickID id) const noexcept {
    return find(id) != nullptr;
}

std::string GamepadManager::name(SDL_JoystickID id) const {
    const Pad* pad = find(id);
    return pad != nullptr ? pad->name : std::string();
}

void GamepadManager::open(SDL_JoystickID id) {
    if (isOpen(id)) {
        return;
    }
    SDL_Gamepad* handle = SDL_OpenGamepad(id);
    if (handle == nullptr) {
        spdlog::warn("Gamepad {} could not be opened: {}", id, SDL_GetError());
        return;
    }
    Pad& pad = m_pads.emplace_back(Pad{id, handle, gamepadName(handle), ++m_useCounter});
    spdlog::info("Gamepad connected: {} (id {}), {} gamepad(s) open", pad.name, id, m_pads.size());
    if (!m_active) {
        m_active = id;
        spdlog::info("Active gamepad: {} (id {})", pad.name, id);
    }
}

void GamepadManager::close(SDL_JoystickID id) {
    const auto it = std::find_if(m_pads.begin(), m_pads.end(), [id](const Pad& pad) { return pad.id == id; });
    if (it == m_pads.end()) {
        return;
    }
    SDL_CloseGamepad(it->handle);
    const std::string name = std::move(it->name);
    m_pads.erase(it);
    spdlog::info("Gamepad disconnected: {} (id {}), {} gamepad(s) open", name, id, m_pads.size());
    if (m_active != id) {
        return;
    }
    m_active.reset();
    const auto next = std::max_element(m_pads.begin(), m_pads.end(),
                                       [](const Pad& a, const Pad& b) { return a.lastUse < b.lastUse; });
    if (next != m_pads.end()) {
        m_active = next->id;
        spdlog::info("Active gamepad: {} (id {})", next->name, next->id);
    }
}

void GamepadManager::markUsed(SDL_JoystickID id) {
    Pad* pad = find(id);
    if (pad == nullptr) {
        return;
    }
    pad->lastUse = ++m_useCounter;
    if (m_active != id) {
        m_active = id;
        spdlog::info("Active gamepad: {} (id {})", pad->name, id);
    }
}

GamepadManager::Pad* GamepadManager::find(SDL_JoystickID id) noexcept {
    const auto it = std::find_if(m_pads.begin(), m_pads.end(), [id](const Pad& pad) { return pad.id == id; });
    return it != m_pads.end() ? &*it : nullptr;
}

const GamepadManager::Pad* GamepadManager::find(SDL_JoystickID id) const noexcept {
    const auto it = std::find_if(m_pads.begin(), m_pads.end(), [id](const Pad& pad) { return pad.id == id; });
    return it != m_pads.end() ? &*it : nullptr;
}

} // namespace rv::platform
