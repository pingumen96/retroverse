#include <rv/platform/sdl_platform.hpp>

#include <atomic>
#include <string>
#include <string_view>
#include <utility>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_version.h>
#include <SDL3/SDL_video.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/main_thread.hpp>

namespace rv::platform {
namespace {

// SDL is process-global state: a second SdlPlatform would SDL_Quit() under the feet of the first.
std::atomic<bool> g_sdlOwned{false};

spdlog::level::level_enum toSpdlogLevel(SDL_LogPriority priority) noexcept {
    switch (priority) {
    case SDL_LOG_PRIORITY_TRACE:
    case SDL_LOG_PRIORITY_VERBOSE:
        return spdlog::level::trace;
    case SDL_LOG_PRIORITY_DEBUG:
        return spdlog::level::debug;
    case SDL_LOG_PRIORITY_INFO:
        return spdlog::level::info;
    case SDL_LOG_PRIORITY_WARN:
        return spdlog::level::warn;
    case SDL_LOG_PRIORITY_ERROR:
        return spdlog::level::err;
    case SDL_LOG_PRIORITY_CRITICAL:
        return spdlog::level::critical;
    default:
        return spdlog::level::info;
    }
}

std::string_view categoryName(int category) noexcept {
    switch (category) {
    case SDL_LOG_CATEGORY_APPLICATION:
        return "application";
    case SDL_LOG_CATEGORY_ERROR:
        return "error";
    case SDL_LOG_CATEGORY_ASSERT:
        return "assert";
    case SDL_LOG_CATEGORY_SYSTEM:
        return "system";
    case SDL_LOG_CATEGORY_AUDIO:
        return "audio";
    case SDL_LOG_CATEGORY_VIDEO:
        return "video";
    case SDL_LOG_CATEGORY_RENDER:
        return "render";
    case SDL_LOG_CATEGORY_INPUT:
        return "input";
    case SDL_LOG_CATEGORY_GPU:
        return "gpu";
    default:
        return "other";
    }
}

void SDLCALL forwardSdlLog(void* /*userdata*/, int category, SDL_LogPriority priority, const char* message) {
    try {
        spdlog::log(toSpdlogLevel(priority), "SDL [{}]: {}", categoryName(category),
                    message != nullptr ? message : "");
    } catch (...) {
        // Called from SDL's C code: an exception must never escape.
    }
}

void restoreSdlLogOutput() noexcept {
    SDL_SetLogOutputFunction(SDL_GetDefaultLogOutputFunction(), nullptr);
}

} // namespace

Expected<SdlPlatform> SdlPlatform::init(const AppMetadata& metadata) {
    core::assertMainThread("SdlPlatform::init");
    bool expected = false;
    if (!g_sdlOwned.compare_exchange_strong(expected, true)) {
        return makeError(ErrorCode::PlatformError, "SDL is already initialized by another SdlPlatform");
    }

    SDL_SetLogOutputFunction(&forwardSdlLog, nullptr);
    if (!SDL_SetAppMetadata(metadata.name.c_str(), metadata.version.c_str(), metadata.identifier.c_str())) {
        spdlog::warn("SDL_SetAppMetadata failed: {}", SDL_GetError());
    }
    // Gamepad input must not reach Retroverse while an emulator (or any other window) has focus.
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0")) {
        spdlog::warn("SDL_SetHint({}) failed: {}", SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, SDL_GetError());
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::string error = SDL_GetError();
        restoreSdlLogOutput();
        g_sdlOwned.store(false);
        return makeError(ErrorCode::PlatformError, "SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) failed: " + error);
    }

    SdlPlatform platform;
    platform.m_active = true;
    return platform;
}

SdlPlatform::SdlPlatform(SdlPlatform&& other) noexcept : m_active(std::exchange(other.m_active, false)) {}

SdlPlatform& SdlPlatform::operator=(SdlPlatform&& other) noexcept {
    if (this != &other) {
        release();
        m_active = std::exchange(other.m_active, false);
    }
    return *this;
}

SdlPlatform::~SdlPlatform() {
    release();
}

void SdlPlatform::release() noexcept {
    if (!m_active) {
        return;
    }
    m_active = false;
    SDL_Quit();
    restoreSdlLogOutput();
    g_sdlOwned.store(false);
}

std::string SdlPlatform::versionString() {
    const int version = SDL_GetVersion();
    const char* revision = SDL_GetRevision();
    return fmt::format("SDL {}.{}.{} ({})", SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version),
                       SDL_VERSIONNUM_MICRO(version), revision != nullptr ? revision : "unknown revision");
}

std::string SdlPlatform::videoDriver() {
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr ? std::string(driver) : std::string("none");
}

} // namespace rv::platform
