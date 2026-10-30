#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/xr/pose.hpp>

namespace rv::xr {

/// A session's state as the runtime reports it (XrSessionState).
enum class SessionState : std::uint8_t {
    Unknown,
    Idle,
    Ready,
    Synchronized,
    Visible,
    Focused,
    Stopping,
    LossPending,
    Exiting,
};

/// "unknown", "idle", "ready", "synchronized", "visible", "focused", "stopping", "loss-pending" or "exiting".
[[nodiscard]] std::string_view toString(SessionState state) noexcept;

/// What a runtime tells its application between frames (xrPollEvent).
struct RuntimeEvent {
    enum class Kind : std::uint8_t {
        StateChanged,
        /// The runtime is going away (XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING): the session ends.
        InstanceLossPending,
        /// The user recentered (or the play area changed): the space's origin moves.
        ReferenceSpaceChangePending,
        RefreshRateChanged,
        /// The runtime's queue overflowed and dropped `count` events.
        EventsLost,
    };
    Kind kind = Kind::StateChanged;
    /// StateChanged: the new state.
    SessionState state = SessionState::Unknown;
    /// RefreshRateChanged: from and to, Hz.
    float fromHz = 0.0f;
    float toHz = 0.0f;
    /// EventsLost: how many.
    std::uint32_t count = 0;
};

/// What xrWaitFrame predicted for the frame about to be rendered.
struct FrameTiming {
    bool shouldRender = false;
    std::int64_t displayTimeNs = 0;
    std::int64_t displayPeriodNs = 0;
};

/// What a runtime and its session are, for the log and the run report.
struct RuntimeDescription {
    /// "fake", or "openxr".
    std::string backend;
    /// XrInstanceProperties: the runtime's name and version.
    std::string runtime;
    std::string runtimeVersion;
    /// XrSystemProperties::systemName: the headset.
    std::string system;
    /// The extensions enabled on the instance.
    std::vector<std::string> extensions;
    /// The reference space the eyes are located in: "local-floor", "stage" or "local".
    std::string space;
    /// How far above the floor the space's origin is: 0 except for LOCAL (an assumed eye height).
    float floorOffset = 0.0f;
    /// The runtime's recommended size of an eye's image, and the size the session renders at (the recommendation
    /// times the render scale, within the runtime's maximum).
    glm::ivec2 recommendedEyeSize{0};
    glm::ivec2 eyeSize{0};
    double renderScale = 1.0;
    /// The swapchains' format ("GL_SRGB8_ALPHA8", ...), and whether the runtime decodes it as sRGB.
    std::string format;
    bool srgb = true;
    std::size_t imagesPerEye = 0;
    /// The refresh rates the headset offers (XR_FB_display_refresh_rate; empty without it), the one asked for.
    std::vector<float> refreshRates;
    std::optional<float> refreshRateRequested;
};

/// A headset's runtime with its session created (ADR-0017 §3): the calls of the session's lifecycle and frame loop,
/// without OpenXR's types. OpenXrRuntime (rv_xr) talks to the system's runtime, FakeRuntime to none. Main thread only;
/// the GL context the session was created on stays current.
class Runtime {
public:
    virtual ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] virtual const RuntimeDescription& description() const noexcept = 0;
    /// The next pending event, or nothing.
    [[nodiscard]] virtual std::optional<RuntimeEvent> pollEvent() = 0;
    /// xrBeginSession, once the session is Ready; xrEndSession once it is Stopping.
    [[nodiscard]] virtual Status beginSession() = 0;
    [[nodiscard]] virtual Status endSession() = 0;
    /// Asks the runtime to end the session: it goes through Stopping, then Exiting.
    virtual void requestExit() = 0;

    /// xrWaitFrame: blocks until the runtime wants the next frame.
    [[nodiscard]] virtual Expected<FrameTiming> waitFrame() = 0;
    [[nodiscard]] virtual Status beginFrame() = 0;
    /// Locates both eyes at `displayTimeNs` in the reference space; false when tracking is lost.
    [[nodiscard]] virtual Expected<bool> locateViews(std::int64_t displayTimeNs, EyeViews& eyes) = 0;
    /// Acquires and waits for the next image of `eye`'s swapchain (0 left, 1 right): its index in images(eye).
    [[nodiscard]] virtual Expected<std::uint32_t> acquireImage(int eye) = 0;
    [[nodiscard]] virtual Status releaseImage(int eye) = 0;
    /// xrEndFrame: both eyes as located last, or no layer without `withViews`.
    [[nodiscard]] virtual Status endFrame(const FrameTiming& timing, bool withViews) = 0;

    /// The GL texture names of `eye`'s swapchain images.
    [[nodiscard]] virtual std::span<const std::uint32_t> images(int eye) const = 0;
    /// The refresh rate now, when the runtime tells it.
    [[nodiscard]] virtual std::optional<float> currentRefreshRate() const = 0;

protected:
    Runtime() = default;
};

} // namespace rv::xr
