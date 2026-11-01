#include "app_input.hpp"

#include <cmath>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/app/binding_rules.hpp>
#include <rv/app/settings_store.hpp>
#include <rv/core/json.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/settings_file.hpp>
#include <rv/core/text.hpp>
#include <rv/platform/graphics_window.hpp>

namespace rv::app {
namespace {

constexpr std::string_view kDefaultBindingsPath = "config/default_bindings.json";
constexpr std::string_view kUserBindingsFile = "bindings.json";

} // namespace

LoadedBindings loadBindingLayers(const core::Paths& paths, bool safe) {
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
    LoadedBindings layers;
    layers.shipped = map;

    const std::filesystem::path userPath = paths.configDir() / core::pathFromUtf8(kUserBindingsFile);
    std::error_code error;
    if (!std::filesystem::is_regular_file(userPath, error)) {
        spdlog::debug("No user bindings at {}", core::pathToUtf8(userPath));
    } else if (const auto document = core::readJsonFile(userPath); !document) {
        spdlog::warn("User bindings ignored: {}", document.error().toString());
    } else if (const auto replaced = map.applyOverrides(*document, core::pathToUtf8(userPath)); !replaced) {
        spdlog::warn("User bindings ignored: {}", replaced.error().toString());
    } else {
        spdlog::info("User bindings from {} replace the bindings of {} action(s)", core::pathToUtf8(userPath),
                     *replaced);
    }

    layers.below = map;
    // settings.json over the user's file (ADR-0018 §2): an action the screen did not touch stays as it is written.
    if (safe) {
        spdlog::warn("--safe-bindings: settings.json's bindings are left out");
    } else {
        const std::size_t remapped = applySettingsBindings(map, core::SettingsFile::loadBeside(paths.configDir()));
        if (remapped > 0) {
            spdlog::info("settings.json replaces the bindings of {} action(s)", remapped);
        }
    }
    for (const input::Action action : keepUnlocked(map, layers.shipped)) {
        spdlog::warn("Input bindings: {} gets its shipped binding back: it may never be left without one",
                     input::actionName(action));
    }
    layers.map = std::move(map);
    return layers;
}

input::ActionMap loadBindings(const core::Paths& paths, bool safe) {
    return loadBindingLayers(paths, safe).map;
}

AppInput::AppInput(platform::GraphicsWindow& window, input::ActionMap bindings,
                   std::optional<input::InputScript> script)
    : m_capture(window.handle()), m_system(bindings), m_controllers(std::move(bindings)) {
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
    if (m_script) {
        m_script->applyRaw(frameIndex, raw);
    }
    m_padConnected = raw.gamepadConnected; // a headset's controllers are no pad to lose
    if (controllers != nullptr) {
        controllers->mergeInto(raw);
    }
    m_raw = raw;
    m_mouseWheel = std::isfinite(raw.mouseWheel) ? raw.mouseWheel : 0.0f;
    input::ActionState actions = m_system.resolve(raw);
    if (m_script) {
        m_script->apply(frameIndex, actions, m_system.settings().pressThreshold);
    }
    return actions;
}

input::ActionState AppInput::controllersAlone(const xr::VirtualPad& controllers) {
    input::RawInputFrame raw;
    controllers.mergeInto(raw);
    m_controllers.setSettings(m_system.settings()); // the overlay edits the devices' settings
    return m_controllers.resolve(raw);
}

void AppInput::setBindings(const input::ActionMap& bindings) {
    m_system.setMap(bindings);
    m_controllers.setMap(bindings);
    // What is down now was pressed under the old map: it must not press what it is bound to from now on (the very key a
    // capture took would activate the row it was captured for).
    const input::ActionState held = m_system.resolve(m_raw);
    if (m_script) {
        m_script->noteHeld(held);
    }
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
