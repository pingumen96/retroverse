#include "app_input.hpp"

#include <cmath>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/core/json.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/platform/graphics_window.hpp>

namespace rv::app {
namespace {

constexpr std::string_view kDefaultBindingsPath = "config/default_bindings.json";
constexpr std::string_view kUserBindingsFile = "bindings.json";

} // namespace

input::ActionMap loadBindings(const core::Paths& paths) {
    const std::filesystem::path defaultsPath =
        (paths.assetRoot() / core::pathFromUtf8(kDefaultBindingsPath)).make_preferred();
    input::ActionMap map;
    if (auto loaded = input::ActionMap::loadFromFile(defaultsPath)) {
        map = std::move(*loaded);
        spdlog::info("Input bindings loaded from {}", core::pathToUtf8(defaultsPath));
    } else {
        map = input::ActionMap::defaults();
        spdlog::warn("Input bindings unavailable, using the built-in defaults: {}", loaded.error().toString());
    }

    const std::filesystem::path userPath = paths.configDir() / core::pathFromUtf8(kUserBindingsFile);
    std::error_code error;
    if (!std::filesystem::is_regular_file(userPath, error)) {
        spdlog::debug("No user bindings at {}", core::pathToUtf8(userPath));
        return map;
    }
    const auto document = core::readJsonFile(userPath);
    if (!document) {
        spdlog::warn("User bindings ignored: {}", document.error().toString());
        return map;
    }
    const auto replaced = map.applyOverrides(*document, core::pathToUtf8(userPath));
    if (!replaced) {
        spdlog::warn("User bindings ignored: {}", replaced.error().toString());
        return map;
    }
    spdlog::info("User bindings from {} replace the bindings of {} action(s)", core::pathToUtf8(userPath), *replaced);
    return map;
}

AppInput::AppInput(platform::GraphicsWindow& window, input::ActionMap bindings,
                   std::optional<input::InputScript> script)
    : m_capture(window.handle()), m_system(std::move(bindings)) {
    if (script) {
        m_script.emplace(std::move(*script));
    }
    m_collector.gamepads().openConnected();
    spdlog::info("Input: {} gamepad(s) connected", m_collector.gamepads().count());
}

void AppInput::handleEvent(const SDL_Event& event) {
    const bool consumed = m_capture.handleEvent(event);
    m_collector.setMouseCaptured(m_capture.captured());
    if (!consumed) {
        m_collector.handleEvent(event);
    }
}

input::ActionState AppInput::update(std::uint64_t frameIndex, const xr::VirtualPad* controllers) {
    input::RawInputFrame raw = m_collector.endFrame();
    if (controllers != nullptr) {
        controllers->mergeInto(raw);
    }
    m_mouseWheel = std::isfinite(raw.mouseWheel) ? raw.mouseWheel : 0.0f;
    input::ActionState actions = m_system.resolve(raw);
    if (m_script) {
        m_script->apply(frameIndex, actions, m_system.settings().pressThreshold);
    }
    return actions;
}

void AppInput::captureMouseWhenFocused(std::string_view reason) {
    m_capture.captureWhenFocused(reason);
    m_collector.setMouseCaptured(m_capture.captured());
}

void AppInput::releaseMouse(std::string_view reason) {
    m_capture.setCaptured(false, reason);
    m_collector.setMouseCaptured(m_capture.captured());
}

} // namespace rv::app
