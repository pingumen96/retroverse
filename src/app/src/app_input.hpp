#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <rv/app/bindings.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/input_script.hpp>
#include <rv/input/input_system.hpp>
#include <rv/platform/mouse_capture.hpp>
#include <rv/platform/sdl_input_collector.hpp>

union SDL_Event;

namespace rv::platform {
class GraphicsWindow;
} // namespace rv::platform

namespace rv::app {

/// Input pipeline of RetroverseApp, one update() per frame on the main thread:
/// SDL events -> platform::SdlInputCollector -> input::RawInputFrame -> input::InputSystem -> input::ActionState ->
/// --input-script overrides (input::InputScriptPlayer). Owns the gamepads (through the collector) and the mouse capture
/// of the window, which must outlive it.
class AppInput {
public:
    /// Opens the gamepads already connected. Without `script`, the device input is used as is.
    AppInput(platform::GraphicsWindow& window, input::ActionMap bindings, std::optional<input::InputScript> script);
    AppInput(const AppInput&) = delete;
    AppInput& operator=(const AppInput&) = delete;

    /// Routes one SDL event: the mouse capture policy first (the click that captures goes no further), then the
    /// collector.
    void handleEvent(const SDL_Event& event);
    /// The actions of frame `frameIndex` (AppContext::frameIndex()): device input resolved through the bindings, with
    /// the script's values on the frames it covers.
    [[nodiscard]] input::ActionState update(std::uint64_t frameIndex);

    /// See platform::MouseCapture::captureWhenFocused(); `reason` completes the log line.
    void captureMouseWhenFocused(std::string_view reason);
    /// Releases the mouse; `reason` completes the log line.
    void releaseMouse(std::string_view reason);
    [[nodiscard]] bool mouseCaptured() const noexcept { return m_capture.captured(); }

    /// The --input-script being played, or nullptr.
    [[nodiscard]] const input::InputScript* script() const noexcept { return m_script ? &m_script->script() : nullptr; }
    /// The bindings and input settings (the developer overlay edits the mouse sensitivity and Y inversion).
    [[nodiscard]] input::InputSystem& system() noexcept { return m_system; }

private:
    platform::SdlInputCollector m_collector;
    platform::MouseCapture m_capture;
    input::InputSystem m_system;
    std::optional<input::InputScriptPlayer> m_script;
};

} // namespace rv::app
