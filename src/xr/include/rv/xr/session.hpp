#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/xr/frame_stats.hpp>
#include <rv/xr/pose.hpp>
#include <rv/xr/runtime.hpp>

namespace rv::xr {

/// A frame the runtime wants: begun, both eyes located and their images acquired.
struct Frame {
    FrameTiming timing;
    /// Both eyes were located (tracking valid). Without them the frame is submitted with no layer.
    bool located = false;
    /// In the reference space.
    EyeViews eyes{};
    /// The GL textures to render the eyes into (left, right), acquired when the runtime wants the frame rendered
    /// (`timing.shouldRender`) and the eyes were located; 0 otherwise.
    std::array<std::uint32_t, 2> images{};
    glm::ivec2 eyeSize{0};
    /// The images are decoded as sRGB: the renderer's display-ready bytes go in verbatim.
    bool srgb = true;
    float floorOffset = 0.0f;

    /// The eyes are to be rendered into `images`.
    [[nodiscard]] bool wantsViews() const noexcept { return images[0] != 0 && images[1] != 0; }
};

/// What happened to the session, in order: a state change or a runtime event, at a frame (the frames waited for
/// before it).
struct SessionEvent {
    std::uint64_t frame = 0;
    std::string what;
};

/// The session of a Runtime and its frame loop (ADR-0017 §3): begins the session when the runtime says it is ready,
/// ends it when it stops, and counts it ended when it exits or is lost (the app then goes back to the desktop).
/// While it runs, each frame is beginFrame() (wait, begin, locate, acquire), the eyes rendered into the frame's
/// images, then endFrame(). Every frame's timing is kept for frameSummary(). Main thread only.
class Session {
public:
    /// The frames frameSummary() covers at most: the first hour at 120 Hz (about 17 MB of records).
    static constexpr std::size_t kMaxFrameRecords = 432'000;

    explicit Session(std::unique_ptr<Runtime> runtime);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] const RuntimeDescription& description() const noexcept;

    /// Handles the runtime's pending events.
    void pollEvents();

    /// Begun and not stopping: frames are waited for and submitted.
    [[nodiscard]] bool running() const noexcept { return m_running; }
    /// Exited or lost: the session is over.
    [[nodiscard]] bool ended() const noexcept { return m_ended; }
    [[nodiscard]] SessionState state() const noexcept { return m_state; }
    /// The session was focused at least once (the app had the headset's input).
    [[nodiscard]] bool reachedFocused() const noexcept { return m_reachedFocused; }
    /// Why the session ended ("exited", "lost", "instance lost", or a failed call), empty while it has not.
    [[nodiscard]] const std::string& endReason() const noexcept { return m_endReason; }

    /// While running: waits for the next frame, begins it, locates the eyes at its display time and, when it is to be
    /// rendered, acquires both images. Nothing when not running. A failure ends the session (endReason()).
    [[nodiscard]] Expected<std::optional<Frame>> beginFrame();
    /// Ends the frame beginFrame() began: releases its images and submits both eyes when `rendered`, or no layer.
    /// Nothing when no frame was begun. A failure ends the session.
    [[nodiscard]] Status endFrame(bool rendered);
    /// Asks the runtime to end the session (when the app quits).
    void requestExit();

    /// The frames waited for so far.
    [[nodiscard]] std::uint64_t framesWaited() const noexcept { return m_framesWaited; }
    [[nodiscard]] FrameSummary frameSummary() const;
    [[nodiscard]] const std::vector<SessionEvent>& events() const noexcept { return m_events; }
    [[nodiscard]] std::optional<float> currentRefreshRate() const;

private:
    void record(std::string what);
    void end(std::string reason);
    /// A failed call: the session ends with it as its reason.
    [[nodiscard]] Error fail(const Error& error);

    std::unique_ptr<Runtime> m_runtime;
    SessionState m_state = SessionState::Unknown;
    bool m_running = false;
    bool m_ended = false;
    bool m_reachedFocused = false;
    std::string m_endReason;
    std::uint64_t m_framesWaited = 0;
    std::vector<SessionEvent> m_events;
    std::vector<FrameRecord> m_frames;
    /// The frame between beginFrame() and endFrame(), and when xrWaitFrame returned it.
    std::optional<Frame> m_frame;
    std::chrono::steady_clock::time_point m_waited;
};

} // namespace rv::xr
