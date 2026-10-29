#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <spike_xr/xr_math.hpp>

namespace rv::spikes::xr {

/// The reference space the eyes are located in (the room's origin is its origin).
enum class SpaceChoice : std::uint8_t {
    Auto,       ///< LOCAL_FLOOR when offered, else STAGE, else LOCAL
    LocalFloor, ///< the floor under the head when the session started, facing where it faced
    Stage,      ///< the floor at the centre of the play area the user set up
    Local,      ///< the head when the session started (the room is drawn 1.6 m lower)
};

struct InstanceOptions {
    /// A runtime's manifest (XR_RUNTIME_JSON, read by the loader): measure a runtime that is not the active one
    /// without changing the system's choice. Empty: the active runtime.
    std::filesystem::path runtimeJson;
};

struct SessionOptions {
    /// The eyes' size over the runtime's recommendation (clamped to its maximum).
    double renderScale = 1.0;
    /// A refresh rate to ask for (XR_FB_display_refresh_rate, when the runtime offers it).
    std::optional<float> refreshRate;
    SpaceChoice space = SpaceChoice::Auto;
};

/// An eye as xrLocateViews found it at a display time.
struct EyeView {
    EyePose pose;
    EyeFov fov;
};

/// What xrWaitFrame predicted for the frame about to be rendered.
struct FrameTiming {
    bool shouldRender = false;
    std::int64_t displayTimeNs = 0;
    std::int64_t displayPeriodNs = 0;
};

/// The spike's OpenXR session on the current OpenGL context (XR_KHR_opengl_enable on Win32): the instance, the
/// headset, the session and its state machine, a reference space, one swapchain per eye, the frame loop's calls. Every
/// step records what it found in the report it is given. The OpenXR types stay in the implementation. Main thread
/// only, with the GL context current from createSession() on.
class OpenXrSession {
public:
    ~OpenXrSession();
    OpenXrSession(const OpenXrSession&) = delete;
    OpenXrSession& operator=(const OpenXrSession&) = delete;

    /// Loads the runtime and creates the instance with XR_KHR_opengl_enable (required) and the optional extensions the
    /// spike uses (XR_FB_display_refresh_rate, XR_EXT_local_floor). Records the runtime and every extension it
    /// offers under report["runtime"]. Errors: Unavailable (no runtime installed or active), Unsupported (no OpenGL).
    [[nodiscard]] static Expected<std::unique_ptr<OpenXrSession>> createInstance(const InstanceOptions& options,
                                                                                 nlohmann::json& report);

    /// Finds the headset, retrying for up to `wait` while the runtime reports none connected (Link not started yet).
    /// Records the system, its stereo views, blend modes and OpenGL requirements under report["system"]. Errors:
    /// Unavailable when no headset turned up.
    [[nodiscard]] Status findSystem(std::chrono::milliseconds wait, nlohmann::json& report);

    /// Creates the session on the OpenGL context current on this thread, the reference space and the eyes'
    /// swapchains. Records them under report["session"].
    [[nodiscard]] Status createSession(const SessionOptions& options, nlohmann::json& report);

    /// Handles the runtime's events: begins the session when it is ready, ends it when it stops. Every state change
    /// and notable event goes to the event timeline.
    void pollEvents();

    /// The session is running: frames may be waited for and submitted.
    [[nodiscard]] bool running() const noexcept { return m_running; }
    /// The session has ended (exiting, lost, or the instance is going away): stop the loop.
    [[nodiscard]] bool ended() const noexcept { return m_ended; }
    /// The session has been focused at least once (the app had the headset's input).
    [[nodiscard]] bool reachedFocused() const noexcept { return m_reachedFocused; }
    [[nodiscard]] const nlohmann::json& events() const noexcept { return m_events; }

    [[nodiscard]] Expected<FrameTiming> waitFrame();
    [[nodiscard]] Status beginFrame();
    /// Locates both eyes at `displayTimeNs`; false (and nothing submitted for them later) when tracking is lost.
    [[nodiscard]] Expected<bool> locateViews(std::int64_t displayTimeNs, std::array<EyeView, 2>& eyes);
    /// Acquires and waits for the next image of `eye`'s swapchain: its index in images(eye).
    [[nodiscard]] Expected<std::uint32_t> acquireImage(int eye);
    [[nodiscard]] Status releaseImage(int eye);
    /// Submits the frame: both eyes as located by the last locateViews() when `withViews`, no layer otherwise.
    [[nodiscard]] Status endFrame(const FrameTiming& timing, bool withViews);

    /// Asks the runtime to end the session (it goes through STOPPING and EXITING).
    void requestExit();

    [[nodiscard]] glm::ivec2 eyeSize() const noexcept { return m_eyeSize; }
    [[nodiscard]] bool srgb() const noexcept { return m_srgb; }
    /// The GL texture names of `eye`'s swapchain images.
    [[nodiscard]] const std::vector<std::uint32_t>& images(int eye) const { return m_images[eye]; }
    /// How far above the floor the reference space's origin is: 0 except for LOCAL (1.6 m, an assumed eye height).
    [[nodiscard]] float floorOffset() const noexcept { return m_floorOffset; }
    /// The refresh rate the runtime reports now (XR_FB_display_refresh_rate); nothing without the extension.
    [[nodiscard]] std::optional<float> currentRefreshRate() const;

private:
    struct Native;

    OpenXrSession();
    void recordEvent(nlohmann::json event);

    std::unique_ptr<Native> m_native;
    std::chrono::steady_clock::time_point m_origin;
    nlohmann::json m_events = nlohmann::json::array();
    glm::ivec2 m_eyeSize{0};
    bool m_srgb = false;
    float m_floorOffset = 0.0f;
    std::array<std::vector<std::uint32_t>, 2> m_images;
    bool m_running = false;
    bool m_ended = false;
    bool m_reachedFocused = false;
};

[[nodiscard]] const char* toString(SpaceChoice space) noexcept;

} // namespace rv::spikes::xr
