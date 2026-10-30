#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <rv/app/command_line.hpp>
#include <rv/app/run_report.hpp>
#include <rv/xr/session.hpp>

namespace rv::xr {
class FakeRuntime;
} // namespace rv::xr

namespace rv::app {

/// The VR mode's session in a run (I10 step 2, ADR-0017 §3, §8): the runtime --vr asks for on the GL context current
/// on the main thread (the system's OpenXR runtime, or xr::FakeRuntime with its swapchain images as GL textures and
/// the --input-script's head poses), its events and its frames. AppHost drives it: pollEvents() every loop, then
/// beginFrame() before the logic's update, endFrame() after its render. The session ending (exited, lost, a failed
/// call) sends the run back to the desktop. Main thread only; destroy it before the GL context.
class XrSessionDriver {
public:
    /// Starts the session, or returns nothing when VR is unavailable (no runtime, no headset, OpenGL refused, a build
    /// without the OpenXR loader): logged, and recorded in `record` (XrRecord::unavailable).
    [[nodiscard]] static std::unique_ptr<XrSessionDriver> start(const CommandLineOptions& options, XrRecord& record);
    ~XrSessionDriver();
    XrSessionDriver(const XrSessionDriver&) = delete;
    XrSessionDriver& operator=(const XrSessionDriver&) = delete;

    /// Handles the runtime's pending events.
    void pollEvents();
    /// The session has ended: the run is on the desktop from now on.
    [[nodiscard]] bool ended() const noexcept { return m_session->ended(); }
    /// The frame the runtime wants next, when the session runs: begun, the eyes located and their images acquired.
    /// Nothing when it does not run yet or any more (a failure is logged and ends the session).
    [[nodiscard]] std::optional<xr::Frame> beginFrame();
    /// Submits the frame beginFrame() returned: both eyes when `rendered`, no layer otherwise.
    void endFrame(bool rendered);
    /// Asks the runtime to end the session (the run quits).
    void requestExit();
    /// The session's description, frames and events into `record`.
    void finish(XrRecord& record) const;

private:
    XrSessionDriver(std::unique_ptr<xr::Session> session, const xr::FakeRuntime* fake);

    std::unique_ptr<xr::Session> m_session;
    /// The fake runtime under the session, for its counts; null with OpenXR.
    const xr::FakeRuntime* m_fake = nullptr;
};

} // namespace rv::app
