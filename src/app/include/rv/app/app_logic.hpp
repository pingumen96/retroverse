#pragma once

#include <cstdint>

#include <glm/vec2.hpp>

#include <rv/app/command_line.hpp>
#include <rv/core/expected.hpp>

union SDL_Event;

namespace rv::core {
class FrameClock;
class Paths;
} // namespace rv::core

namespace rv::platform {
class GraphicsWindow;
} // namespace rv::platform

namespace rv::gl {
class GLDevice;
} // namespace rv::gl

namespace rv::app {

class AppHost;

/// What the host exposes to the application logic during a run. Created by AppHost once the window, the OpenGL
/// context and the GL device exist; valid until IAppLogic::onShutdown returns. Main thread only.
class AppContext {
public:
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    [[nodiscard]] const CommandLineOptions& options() const noexcept { return m_options; }
    [[nodiscard]] const core::Paths& paths() const noexcept { return m_paths; }
    [[nodiscard]] platform::GraphicsWindow& window() noexcept { return m_window; }
    [[nodiscard]] gl::GLDevice& device() noexcept { return m_device; }
    /// Ticked by the host once per frame, before onUpdate.
    [[nodiscard]] const core::FrameClock& clock() const noexcept { return m_clock; }
    /// Zero-based index of the frame being updated and rendered (= frames presented so far).
    [[nodiscard]] std::uint64_t frameIndex() const noexcept { return m_frameIndex; }
    /// Default framebuffer size in pixels; follows window resizes and fullscreen changes.
    [[nodiscard]] glm::ivec2 framebufferSize() const noexcept { return m_framebufferSize; }

    /// Ends the main loop once the current frame is done.
    void requestQuit() noexcept { m_quitRequested = true; }
    [[nodiscard]] bool quitRequested() const noexcept { return m_quitRequested; }

private:
    friend class AppHost;

    AppContext(const CommandLineOptions& options, const core::Paths& paths, platform::GraphicsWindow& window,
               gl::GLDevice& device, core::FrameClock& clock) noexcept
        : m_options(options), m_paths(paths), m_window(window), m_device(device), m_clock(clock) {}

    const CommandLineOptions& m_options;
    const core::Paths& m_paths;
    platform::GraphicsWindow& m_window;
    gl::GLDevice& m_device;
    core::FrameClock& m_clock;
    std::uint64_t m_frameIndex = 0;
    glm::ivec2 m_framebufferSize{0};
    bool m_quitRequested = false;
};

/// Application behavior driven by AppHost. Call order: onInit once; then per frame onEvent for each pending event,
/// onUpdate, onRender (followed by the buffer swap); finally onShutdown exactly once, also when onInit failed or an
/// exception ended the run. The OpenGL context is current in every callback and destroyed only after onShutdown, which
/// must release every GPU resource the logic owns.
class IAppLogic {
public:
    virtual ~IAppLogic();

    /// Creates the logic's resources. An error aborts the run with ExitCode::InitializationFailed.
    [[nodiscard]] virtual Status onInit(AppContext& context) = 0;
    /// Every SDL event, after the host has handled quit, close, resize, Escape and Alt+Enter.
    virtual void onEvent(AppContext& /*context*/, const SDL_Event& /*event*/) {}
    /// Simulation step; `deltaSeconds` is the frame delta clamped to 0.1 s.
    virtual void onUpdate(AppContext& context, double deltaSeconds) = 0;
    /// Draws the frame into the default framebuffer (start with GLDevice::beginFrame).
    virtual void onRender(AppContext& context) = 0;
    virtual void onShutdown(AppContext& /*context*/) {}
};

} // namespace rv::app
