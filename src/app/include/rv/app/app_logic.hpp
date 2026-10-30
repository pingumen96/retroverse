#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include <glm/vec2.hpp>

#include <rv/app/command_line.hpp>
#include <rv/app/exit_code.hpp>
#include <rv/app/frame_timing.hpp>
#include <rv/app/run_report.hpp>
#include <rv/core/expected.hpp>
#include <rv/xr/session.hpp>
#include <rv/xr/xr_settings.hpp>

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

    /// Ends the main loop once the current frame is done and makes the run exit with ExitCode::RuntimeFailure, for
    /// failures that must not pass silently, such as a --screenshot that was not written. Also honored from
    /// IAppLogic::onShutdown when the main loop ended normally: the host calls it before deciding the exit code. The
    /// host logs the error; only the first one is kept, with its `code` (ExitCode::InitializationFailed for a start the
    /// run could not make after its initialization: I4 step 5a's --start-room of a room the building lacks).
    void failRun(Error error, ExitCode code = ExitCode::RuntimeFailure) {
        if (!m_failure) {
            m_failure = std::move(error);
            m_failureCode = code;
        }
        m_quitRequested = true;
    }
    [[nodiscard]] const std::optional<Error>& failure() const noexcept { return m_failure; }

    /// The report of the run (--run-report), which the host writes when the run ends: the logic adds what it rendered
    /// (RunReport documents who fills which field). Kept, and harmless to fill, without --run-report too.
    [[nodiscard]] RunReport& report() noexcept { return m_report; }
    /// The frame times of the run and its phases: the logic opens and closes a phase around what it runs over several
    /// frames (a refresh, a cover load, an animation: kPhase*), and the run report gives each phase's frame times. The
    /// host records every frame it presents.
    [[nodiscard]] FrameTiming& frameTiming() noexcept { return m_frameTiming; }

    /// The idle mode, the host's side of ExternalEmulatorRunning (specification §15.5; ADR-0006: the window kept behind
    /// the emulator renders nothing). While it is set the host updates, renders and presents nothing: it waits for
    /// events (at most AppHost::kIdleWait at a time, so that the logic can follow timeouts) and calls
    /// IAppLogic::onIdle after each wait, events still reaching onEvent first. Frames do not advance: frameIndex() stays
    /// where it was and a frame-limited run's budget (--frames) is not consumed by the wait. The first onIdle comes
    /// right after the frame during which the logic set it was presented. Set and cleared by the logic, from any
    /// callback; the first frame after it gets a short delta (the wait does not count as frame time).
    void setIdle(bool idle) noexcept { m_idle = idle; }
    [[nodiscard]] bool idle() const noexcept { return m_idle; }
    /// Wakes the host's idle wait from any thread: it pushes the host's wake event (an SDL user event, which the host
    /// keeps to itself), so that the logic's next onIdle comes at once. For the process runner's wake callback
    /// (launcher::StartCallbacks::wake). Valid for the run; harmless while the host is not waiting, and a no-op when SDL
    /// had no event type left to register.
    [[nodiscard]] const std::function<void()>& wakeCallback() const noexcept { return m_wake; }

    /// The VR frame being updated and rendered (--vr, I10 step 2, ADR-0017 §3): the eyes the runtime located for its
    /// display time, in the tracking space, and the swapchain images to render them into. Null on the desktop, while
    /// the session does not run yet, and once it has ended (the run is back on the desktop).
    [[nodiscard]] const xr::Frame* xrFrame() const noexcept { return m_xrFrame ? &*m_xrFrame : nullptr; }
    /// The logic rendered both eyes into the frame's images (xr::Frame::images): the host submits them with it.
    void setXrViewsRendered() noexcept { m_xrViewsRendered = true; }
    /// The run is in VR (--vr, or xr.json's display.mode; I10 step 3): its session started before onInit and has not
    /// ended. False on the desktop, and once the run went back to it.
    [[nodiscard]] bool vr() const noexcept { return m_vr; }
    /// xr.json as the run read it (its shipped defaults when the user's file was left out).
    [[nodiscard]] const xr::XrSettings& xrSettings() const noexcept { return m_xrSettings; }
    /// Vibrates `hand`'s controller at `amplitude` ([0, 1]) for `seconds` (I10 step 4): the host passes it to the VR
    /// session after the frame's update; nothing on the desktop.
    void vibrate(xr::Hand hand, float amplitude, float seconds) {
        if (m_vr) {
            m_xrVibrations.push_back(XrVibration{hand, amplitude, seconds});
        }
    }

private:
    friend class AppHost;

    AppContext(const CommandLineOptions& options, const core::Paths& paths, platform::GraphicsWindow& window,
               gl::GLDevice& device, core::FrameClock& clock, RunReport& report, FrameTiming& frameTiming) noexcept
        : m_options(options), m_paths(paths), m_window(window), m_device(device), m_clock(clock), m_report(report),
          m_frameTiming(frameTiming) {}

    const CommandLineOptions& m_options;
    const core::Paths& m_paths;
    platform::GraphicsWindow& m_window;
    gl::GLDevice& m_device;
    core::FrameClock& m_clock;
    RunReport& m_report;
    FrameTiming& m_frameTiming;
    std::uint64_t m_frameIndex = 0;
    glm::ivec2 m_framebufferSize{0};
    bool m_quitRequested = false;
    bool m_idle = false;
    /// The host's wake event (SDL_RegisterEvents; 0: none) and the callback that pushes it.
    std::uint32_t m_wakeEvent = 0;
    std::function<void()> m_wake = [] {};
    std::optional<xr::Frame> m_xrFrame;
    bool m_xrViewsRendered = false;
    bool m_vr = false;
    xr::XrSettings m_xrSettings;
    struct XrVibration {
        xr::Hand hand = xr::Hand::Right;
        float amplitude = 0.0f;
        float seconds = 0.0f;
    };
    std::vector<XrVibration> m_xrVibrations;
    std::optional<Error> m_failure;
    ExitCode m_failureCode = ExitCode::RuntimeFailure;
};

/// Application behavior driven by AppHost. Call order: onInit once; then per frame onEvent for each pending event,
/// onUpdate, onRender (followed by the buffer swap); while the logic keeps the host idle (AppContext::setIdle), onEvent
/// for each pending event and onIdle after each wait, instead of the frames; while an interactive run's window is
/// minimized (or --simulate-minimized), onEvent and onMinimized after each wait instead; finally onShutdown exactly
/// once, also when onInit failed or an exception ended the run (the quit hook of a logic that idles: the host may stop
/// while it does).
/// The OpenGL context is current in every callback and destroyed only after onShutdown, which must release every GPU
/// resource the logic owns.
class IAppLogic {
public:
    virtual ~IAppLogic();

    /// Creates the logic's resources. An error aborts the run with ExitCode::InitializationFailed.
    [[nodiscard]] virtual Status onInit(AppContext& context) = 0;
    /// Every SDL event, after the host has handled quit, close and resize, except the Alt+Enter press, which the host
    /// consumes to toggle fullscreen (platform::isFullscreenToggle).
    virtual void onEvent(AppContext& /*context*/, const SDL_Event& /*event*/) {}
    /// Simulation step; `deltaSeconds` is the frame delta clamped to 0.1 s, or the --fixed-dt step
    /// (CommandLineOptions::fixedDeltaSeconds) when given.
    virtual void onUpdate(AppContext& context, double deltaSeconds) = 0;
    /// Renders the frame and leaves it in the default framebuffer, which the host presents afterwards (typically:
    /// GLDevice::beginPass on an offscreen Framebuffer, draws, Framebuffer::blitToDefault).
    virtual void onRender(AppContext& context) = 0;
    /// While the host idles (AppContext::setIdle): once right after the frame that set it was presented, then after
    /// every wait (an event, the wake callback, AppHost::kIdleWait passing). Clearing the idle mode here makes the host
    /// render the next frame at once.
    virtual void onIdle(AppContext& /*context*/) {}
    /// While the window is minimized and no frame renders (an interactive run; a frame-limited one renders on unless
    /// --simulate-minimized): after each of the host's waits, `elapsedSeconds` its length (I5 step 2's review: the
    /// screens' decoders pause and stop, the priority goes back to normal). Not while the host idles.
    virtual void onMinimized(AppContext& /*context*/, double /*elapsedSeconds*/) {}
    virtual void onShutdown(AppContext& /*context*/) {}
};

} // namespace rv::app
