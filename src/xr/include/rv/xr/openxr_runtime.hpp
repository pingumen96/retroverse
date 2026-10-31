#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include <rv/core/expected.hpp>
#include <rv/xr/runtime.hpp>
#include <rv/xr/xr_bindings.hpp>

namespace rv::xr {

/// The reference space the eyes are located in (the tracking space's origin).
enum class SpaceChoice : std::uint8_t {
    Auto,       ///< LOCAL_FLOOR when offered, else STAGE, else LOCAL
    LocalFloor, ///< the floor under the head when the session started, facing where it faced
    Stage,      ///< the floor at the centre of the play area the user set up
    Local,      ///< the head when the session started (its height assumed)
};

struct OpenXrOptions {
    /// A runtime's manifest (XR_RUNTIME_JSON, read by the loader): a runtime that is not the system's active one,
    /// without changing the system's choice. Empty: the active runtime.
    std::filesystem::path runtimeJson;
    /// How long to wait for a headset the runtime does not see yet (Link not started on it).
    std::chrono::milliseconds headsetWait{0};
    /// The eyes' size over the runtime's recommendation, within its maximum.
    double renderScale = 1.0;
    /// A refresh rate to ask for (XR_FB_display_refresh_rate, when the runtime offers it).
    std::optional<float> refreshRate;
    SpaceChoice space = SpaceChoice::Auto;
    /// The controllers' bindings to suggest (xr_bindings.json, I10 step 4); none: no controller is read.
    XrBindings bindings;
};

/// The system's OpenXR runtime (ADR-0017 §1, §2): an instance with XR_KHR_opengl_enable and the optional extensions
/// (XR_FB_display_refresh_rate, XR_EXT_local_floor), the headset, a session on the OpenGL context current on the
/// calling thread (wglGetCurrentDC/Context), the reference space and one sRGB swapchain per eye. Each step is logged.
/// Errors: Unavailable when no runtime is installed or active or no headset turned up within `headsetWait`,
/// Unsupported when the runtime refuses OpenGL 4.5, the stereo configuration or both swapchain formats, PlatformError
/// for a failed call.
[[nodiscard]] Expected<std::unique_ptr<Runtime>> createOpenXrRuntime(const OpenXrOptions& options);

} // namespace rv::xr
