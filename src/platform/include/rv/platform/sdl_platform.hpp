#pragma once

#include <string>

#include <rv/core/expected.hpp>

namespace rv::platform {

/// Application identity reported to SDL through SDL_SetAppMetadata (used by the OS, e.g. in audio mixers).
struct AppMetadata {
    std::string name;       ///< "Retroverse"
    std::string version;    ///< "0.1.0"
    std::string identifier; ///< reverse-DNS application id
};

/// RAII owner of the SDL library: SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) on creation, SDL_Quit on destruction.
///
/// While it is alive, SDL's own log output is forwarded to the spdlog default logger, and joystick/gamepad events are
/// only delivered while a Retroverse window has focus (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS = "0"), so input
/// meant for a running emulator never moves the camera. At most one instance exists at a time; create it on the main
/// thread before any window and destroy it after the last one.
class SdlPlatform {
public:
    [[nodiscard]] static Expected<SdlPlatform> init(const AppMetadata& metadata);

    SdlPlatform(SdlPlatform&& other) noexcept;
    SdlPlatform& operator=(SdlPlatform&& other) noexcept;
    SdlPlatform(const SdlPlatform&) = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;
    ~SdlPlatform();

    /// Version of the loaded SDL3 library, e.g. "SDL 3.4.18 (release-3.4.18-0-g...)".
    [[nodiscard]] static std::string versionString();
    /// Name of the active video driver ("windows"), or "none" before initialization.
    [[nodiscard]] static std::string videoDriver();

private:
    SdlPlatform() noexcept = default;
    void release() noexcept;

    bool m_active = false;
};

} // namespace rv::platform
