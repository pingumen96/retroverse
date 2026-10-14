#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL_joystick.h>

union SDL_Event;
struct SDL_Gamepad;

namespace rv::platform {

/// The open gamepads of the application (specification §12.1: hot-plug, reconnection, several controllers, priority
/// to the device in use). Main thread only; SDL must stay initialized with SDL_INIT_GAMEPAD (rv::platform::SdlPlatform)
/// for the manager's whole lifetime.
///
///  - SDL_EVENT_GAMEPAD_ADDED opens the pad and logs "Gamepad connected: <name> (id <id>)"; SDL_EVENT_GAMEPAD_REMOVED
///    closes it and logs the disconnection. Opening is idempotent, so openConnected() and the ADDED events SDL sends
///    for pads present at startup do not conflict.
///  - The active pad is the one used most recently: a button press, or an axis pushed past kActivityThreshold, makes
///    its pad active. A pad connected while none is active becomes active at once; when the active pad is removed,
///    the remaining pad connected or used most recently takes over.
/// Joystick events only arrive while a Retroverse window has focus (SdlPlatform disables background joystick
/// events), so input meant for an emulator never reaches the manager.
class GamepadManager {
public:
    /// Normalized axis magnitude past which axis motion counts as using the pad: resting triggers and stick drift
    /// never take the active role.
    static constexpr float kActivityThreshold = 0.5f;

    GamepadManager() = default;
    GamepadManager(const GamepadManager&) = delete;
    GamepadManager& operator=(const GamepadManager&) = delete;
    /// Closes every open pad.
    ~GamepadManager();

    /// Opens every gamepad connected now.
    void openConnected();
    /// Handles SDL_EVENT_GAMEPAD_ADDED and _REMOVED, and the activity of _BUTTON_DOWN and _AXIS_MOTION events of open
    /// pads; every other event is ignored.
    void handleEvent(const SDL_Event& event);
    /// Closes every pad (logged once at debug level).
    void closeAll() noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return m_pads.size(); }
    [[nodiscard]] bool isOpen(SDL_JoystickID id) const noexcept;
    /// The active pad: present whenever at least one pad is open.
    [[nodiscard]] std::optional<SDL_JoystickID> activeId() const noexcept { return m_active; }
    /// Name reported by SDL for an open pad; empty when the pad is not open.
    [[nodiscard]] std::string name(SDL_JoystickID id) const;

private:
    struct Pad {
        SDL_JoystickID id = 0;
        SDL_Gamepad* handle = nullptr;
        std::string name;
        /// Order of the last connection or use: the highest one takes over when the active pad leaves.
        std::uint64_t lastUse = 0;
    };

    void open(SDL_JoystickID id);
    void close(SDL_JoystickID id);
    void markUsed(SDL_JoystickID id);
    [[nodiscard]] Pad* find(SDL_JoystickID id) noexcept;
    [[nodiscard]] const Pad* find(SDL_JoystickID id) const noexcept;

    std::vector<Pad> m_pads; ///< in connection order
    std::optional<SDL_JoystickID> m_active;
    std::uint64_t m_useCounter = 0;
};

} // namespace rv::platform
