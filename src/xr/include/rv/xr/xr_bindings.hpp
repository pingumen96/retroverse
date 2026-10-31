#pragma once

// xr_bindings.json (I10 step 4, ADR-0017 §5): the controllers' inputs for each OpenXR interaction profile, which the
// OpenXR runtime suggests when the VR mode starts. The shipped file is assets/config/xr_bindings.json (Touch, Index,
// Vive, Windows Mixed Reality, Pico 4, the simple controller); the user's <user data>/config/xr_bindings.json, read and
// never written, replaces a profile of the same path or adds one. A user file that cannot be read or checked is left
// out whole (an issue logged).
//
//   {"schemaVersion": 1,
//    "profiles": [{"profile": "/interaction_profiles/oculus/touch_controller",
//                  "extension": "XR_...",                           // optional: suggested only when the runtime has it
//                  "bindings": {"south": ["/user/hand/right/input/a/click"], ...}}, ...]}

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/input/codes.hpp>

namespace rv::xr {

/// The actions of Retroverse's OpenXR action set: a gamepad's sticks, buttons and triggers (PadState), and each hand's
/// select, poses and vibration.
enum class ControllerAction : std::uint8_t {
    LeftStick,
    RightStick,
    South,
    East,
    West,
    North,
    Back,
    Start,
    LeftStickClick,
    RightStickClick,
    LeftShoulder,
    RightShoulder,
    LeftTrigger,
    RightTrigger,
    Select,
    Grip,
    Aim,
    Haptic,
    Count
};

inline constexpr std::size_t kControllerActionCount = static_cast<std::size_t>(ControllerAction::Count);

/// The kind of value an action has (XrActionType).
enum class ControllerActionKind : std::uint8_t { Boolean, Float, Vector2, Pose, Vibration };

struct ControllerActionInfo {
    /// The OpenXR action's name and the key in xr_bindings.json ("left_stick", "south", ..., "haptic").
    std::string_view name;
    /// Shown by the runtime (its bindings screen).
    std::string_view localizedName;
    ControllerActionKind kind = ControllerActionKind::Boolean;
    /// Read per hand (subaction paths /user/hand/left and /user/hand/right): select, the poses, the vibration.
    bool perHand = false;
    /// The gamepad element a pad action drives: a button, or a stick's two axes, or a trigger's axis.
    std::optional<input::GamepadButton> button;
    std::optional<input::GamepadAxis> axisX;
    std::optional<input::GamepadAxis> axisY;
};

[[nodiscard]] const ControllerActionInfo& infoOf(ControllerAction action) noexcept;
[[nodiscard]] std::optional<ControllerAction> controllerActionFromName(std::string_view name) noexcept;

struct ProfileBinding {
    ControllerAction action = ControllerAction::Select;
    /// "/user/hand/<left|right>/input/..." or ".../output/haptic".
    std::string path;

    bool operator==(const ProfileBinding&) const = default;
};

struct InteractionProfile {
    /// "/interaction_profiles/<vendor>/<device>".
    std::string path;
    /// The instance extension the profile needs (XR_BD_controller_interaction for the Pico 4's), if any.
    std::optional<std::string> extension;
    /// By action name, each action's paths in the file's order.
    std::vector<ProfileBinding> bindings;

    bool operator==(const InteractionProfile&) const = default;
};

struct XrBindings {
    std::vector<InteractionProfile> profiles;

    bool operator==(const XrBindings&) const = default;
};

/// The profiles of a document. Errors (ValidationFailed, the JSON pointer in the message): an unknown schema version,
/// action or key; a profile path that does not start with /interaction_profiles/; a binding path that is not a hand's
/// (/user/hand/left/ or /user/hand/right/) or does not fit its action's kind (a stick to a thumbstick or a trackpad, a
/// pose to .../pose, the vibration to .../output/haptic, buttons and triggers to another input); a profile twice.
[[nodiscard]] Expected<XrBindings> parseXrBindings(const nlohmann::json& document, std::string_view context);

struct LoadedXrBindings {
    XrBindings bindings;
    std::vector<Error> issues;
};

/// The shipped profiles under `assetRoot`/config, the user's `userConfigDir`/xr_bindings.json over them. Errors: the
/// shipped file cannot be read or checked.
[[nodiscard]] Expected<LoadedXrBindings> loadXrBindings(const std::filesystem::path& assetRoot,
                                                        const std::filesystem::path& userConfigDir);

} // namespace rv::xr
