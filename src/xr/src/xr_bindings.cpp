#include <rv/xr/xr_bindings.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::xr {
namespace {

using input::GamepadAxis;
using input::GamepadButton;
using Json = nlohmann::json;
using Kind = ControllerActionKind;

constexpr std::string_view kFile = "xr_bindings.json";
constexpr int kSchemaVersion = 1;

const std::array<ControllerActionInfo, kControllerActionCount> kActions = {{
    {"left_stick", "Left stick", Kind::Vector2, false, std::nullopt, GamepadAxis::LeftX, GamepadAxis::LeftY},
    {"right_stick", "Right stick", Kind::Vector2, false, std::nullopt, GamepadAxis::RightX, GamepadAxis::RightY},
    {"south", "A", Kind::Boolean, false, GamepadButton::South, std::nullopt, std::nullopt},
    {"east", "B", Kind::Boolean, false, GamepadButton::East, std::nullopt, std::nullopt},
    {"west", "X", Kind::Boolean, false, GamepadButton::West, std::nullopt, std::nullopt},
    {"north", "Y", Kind::Boolean, false, GamepadButton::North, std::nullopt, std::nullopt},
    {"back", "View", Kind::Boolean, false, GamepadButton::Back, std::nullopt, std::nullopt},
    {"start", "Menu", Kind::Boolean, false, GamepadButton::Start, std::nullopt, std::nullopt},
    {"left_stick_click", "Left stick click", Kind::Boolean, false, GamepadButton::LeftStick, std::nullopt,
     std::nullopt},
    {"right_stick_click", "Right stick click", Kind::Boolean, false, GamepadButton::RightStick, std::nullopt,
     std::nullopt},
    {"left_shoulder", "Left shoulder", Kind::Boolean, false, GamepadButton::LeftShoulder, std::nullopt, std::nullopt},
    {"right_shoulder", "Right shoulder", Kind::Boolean, false, GamepadButton::RightShoulder, std::nullopt,
     std::nullopt},
    {"left_trigger", "Left trigger", Kind::Float, false, std::nullopt, GamepadAxis::LeftTrigger, std::nullopt},
    {"right_trigger", "Right trigger", Kind::Float, false, std::nullopt, GamepadAxis::RightTrigger, std::nullopt},
    {"select", "Select", Kind::Float, true, std::nullopt, std::nullopt, std::nullopt},
    {"grip", "Grip pose", Kind::Pose, true, std::nullopt, std::nullopt, std::nullopt},
    {"aim", "Aim pose", Kind::Pose, true, std::nullopt, std::nullopt, std::nullopt},
    {"haptic", "Vibration", Kind::Vibration, true, std::nullopt, std::nullopt, std::nullopt},
}};

Error invalid(std::string_view context, const std::string& pointer, std::string_view message) {
    return Error{ErrorCode::ValidationFailed, pointer + ": " + std::string(message), std::string(context)};
}

bool endsWith(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

/// Why `path` does not fit `kind`, or nothing.
std::optional<std::string_view> pathProblem(std::string_view path, Kind kind) {
    if (!path.starts_with("/user/hand/left/") && !path.starts_with("/user/hand/right/")) {
        return "expected a hand's path (/user/hand/left/... or /user/hand/right/...)";
    }
    const bool stick = endsWith(path, "/thumbstick") || endsWith(path, "/trackpad");
    switch (kind) {
    case Kind::Vector2:
        return stick ? std::nullopt : std::optional<std::string_view>("expected a thumbstick or a trackpad");
    case Kind::Pose:
        return endsWith(path, "/pose") ? std::nullopt : std::optional<std::string_view>("expected a pose");
    case Kind::Vibration:
        return endsWith(path, "/output/haptic") ? std::nullopt
                                                : std::optional<std::string_view>("expected .../output/haptic");
    case Kind::Boolean:
    case Kind::Float:
        if (path.find("/input/") == std::string_view::npos || stick || endsWith(path, "/pose")) {
            return "expected a button, a trigger or a squeeze";
        }
        return std::nullopt;
    }
    return std::nullopt;
}

Expected<InteractionProfile> parseProfile(const Json& value, const std::string& pointer, std::string_view context) {
    if (!value.is_object()) {
        return tl::unexpected(invalid(context, pointer, "expected an object"));
    }
    for (const auto& [key, member] : value.items()) {
        if (key != "profile" && key != "extension" && key != "bindings") {
            return tl::unexpected(invalid(context, pointer + "/" + key, "unknown key"));
        }
    }
    InteractionProfile profile;
    const auto path = value.find("profile");
    if (path == value.end() || !path->is_string() ||
        !path->get<std::string>().starts_with("/interaction_profiles/")) {
        return tl::unexpected(invalid(context, pointer + "/profile", "expected /interaction_profiles/..."));
    }
    profile.path = path->get<std::string>();
    if (const auto extension = value.find("extension"); extension != value.end()) {
        if (!extension->is_string() || !extension->get<std::string>().starts_with("XR_")) {
            return tl::unexpected(invalid(context, pointer + "/extension", "expected an extension's name, XR_..."));
        }
        profile.extension = extension->get<std::string>();
    }
    const auto bindings = value.find("bindings");
    if (bindings == value.end() || !bindings->is_object() || bindings->empty()) {
        return tl::unexpected(invalid(context, pointer + "/bindings", "expected an object of actions"));
    }
    for (const auto& [name, paths] : bindings->items()) {
        const std::string at = pointer + "/bindings/" + name;
        const std::optional<ControllerAction> action = controllerActionFromName(name);
        if (!action) {
            return tl::unexpected(invalid(context, at, "unknown action"));
        }
        if (!paths.is_array() || paths.empty()) {
            return tl::unexpected(invalid(context, at, "expected an array of paths"));
        }
        for (std::size_t index = 0; index < paths.size(); ++index) {
            const std::string element = fmt::format("{}/{}", at, index);
            if (!paths[index].is_string()) {
                return tl::unexpected(invalid(context, element, "expected a path"));
            }
            std::string text = paths[index].get<std::string>();
            if (const auto problem = pathProblem(text, infoOf(*action).kind)) {
                return tl::unexpected(invalid(context, element, *problem));
            }
            profile.bindings.push_back(ProfileBinding{*action, std::move(text)});
        }
    }
    return profile;
}

} // namespace

const ControllerActionInfo& infoOf(ControllerAction action) noexcept {
    const auto index = static_cast<std::size_t>(action);
    return kActions[index < kActions.size() ? index : 0];
}

std::optional<ControllerAction> controllerActionFromName(std::string_view name) noexcept {
    for (std::size_t index = 0; index < kActions.size(); ++index) {
        if (kActions[index].name == name) {
            return static_cast<ControllerAction>(index);
        }
    }
    return std::nullopt;
}

Expected<XrBindings> parseXrBindings(const nlohmann::json& document, std::string_view context) {
    if (!document.is_object()) {
        return tl::unexpected(invalid(context, "", "expected an object"));
    }
    for (const auto& [key, value] : document.items()) {
        if (key != "schemaVersion" && key != "profiles") {
            return tl::unexpected(invalid(context, "/" + key, "unknown key"));
        }
    }
    const auto version = document.find("schemaVersion");
    if (version == document.end() || !version->is_number_integer() || version->get<int>() != kSchemaVersion) {
        return tl::unexpected(invalid(context, "/schemaVersion", fmt::format("expected {}", kSchemaVersion)));
    }
    const auto profiles = document.find("profiles");
    if (profiles == document.end() || !profiles->is_array()) {
        return tl::unexpected(invalid(context, "/profiles", "expected an array"));
    }
    XrBindings bindings;
    for (std::size_t index = 0; index < profiles->size(); ++index) {
        const std::string pointer = fmt::format("/profiles/{}", index);
        auto profile = parseProfile((*profiles)[index], pointer, context);
        if (!profile) {
            return propagate(profile.error());
        }
        const bool twice = std::any_of(bindings.profiles.begin(), bindings.profiles.end(),
                                       [&](const InteractionProfile& known) { return known.path == profile->path; });
        if (twice) {
            return tl::unexpected(invalid(context, pointer + "/profile", "the profile is there twice"));
        }
        bindings.profiles.push_back(std::move(*profile));
    }
    return bindings;
}

Expected<LoadedXrBindings> loadXrBindings(const std::filesystem::path& assetRoot,
                                          const std::filesystem::path& userConfigDir) {
    const std::filesystem::path shippedPath = assetRoot / "config" / core::pathFromUtf8(kFile);
    auto shippedDocument = core::readJsonFile(shippedPath);
    if (!shippedDocument) {
        return propagate(shippedDocument.error());
    }
    auto shipped = parseXrBindings(*shippedDocument, core::pathToUtf8Lossy(shippedPath));
    if (!shipped) {
        return propagate(shipped.error());
    }
    LoadedXrBindings loaded;
    loaded.bindings = std::move(*shipped);
    const std::filesystem::path userPath = userConfigDir / core::pathFromUtf8(kFile);
    std::error_code ignored;
    if (userConfigDir.empty() || !std::filesystem::exists(userPath, ignored)) {
        return loaded;
    }
    auto userDocument = core::readJsonFile(userPath);
    if (!userDocument) {
        loaded.issues.push_back(userDocument.error());
        return loaded;
    }
    auto user = parseXrBindings(*userDocument, core::pathToUtf8Lossy(userPath));
    if (!user) {
        loaded.issues.push_back(user.error());
        return loaded;
    }
    for (InteractionProfile& profile : user->profiles) {
        const auto known =
            std::find_if(loaded.bindings.profiles.begin(), loaded.bindings.profiles.end(),
                         [&](const InteractionProfile& shippedProfile) { return shippedProfile.path == profile.path; });
        if (known != loaded.bindings.profiles.end()) {
            *known = std::move(profile);
        } else {
            loaded.bindings.profiles.push_back(std::move(profile));
        }
    }
    return loaded;
}

} // namespace rv::xr
