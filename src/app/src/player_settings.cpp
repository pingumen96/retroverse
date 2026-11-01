#include <rv/app/player_settings.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <rv/core/json.hpp>
#include <rv/core/settings_file.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::app {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, 7> kFileKeys = {"schemaVersion",    "horizontalFovDeg", "mouseSensitivity",
                                                       "stickSensitivity", "invertY",          "displayMode",
                                                       "textScale"};
constexpr std::array<std::string_view, 6> kScreenKeys = {"horizontalFovDeg", "mouseSensitivity", "stickSensitivity",
                                                         "invertY",          "displayMode",      "textScale"};

[[nodiscard]] Error failure(ErrorCode code, std::string_view key, std::string_view message, std::string_view context) {
    return Error{code, fmt::format("/{}: {}", key, message), std::string(context)};
}

[[nodiscard]] Expected<double> number(const Json& document, std::string_view key, double low, double high,
                                      std::string_view context) {
    const auto value = document.find(std::string(key));
    if (value == document.end()) {
        return propagate(failure(ErrorCode::ParseError, key, "missing", context));
    }
    if (!value->is_number()) {
        return propagate(failure(ErrorCode::ParseError, key, "expected a number", context));
    }
    const double read = value->get<double>();
    if (!std::isfinite(read) || read < low || read > high) {
        return propagate(failure(ErrorCode::ValidationFailed, key,
                                 fmt::format("expected a number from {:g} to {:g}", low, high), context));
    }
    return read;
}

} // namespace

std::string_view toString(DisplayMode mode) noexcept {
    return mode == DisplayMode::Borderless ? "borderless" : "window";
}

std::span<const std::string_view> playerSettingKeys() noexcept {
    return kScreenKeys;
}

Expected<PlayerSettings> parsePlayerSettings(const Json& document, std::string_view context) {
    if (!document.is_object()) {
        return makeError(ErrorCode::ParseError, "expected an object", std::string(context));
    }
    for (const auto& [key, value] : document.items()) {
        if (std::find(kFileKeys.begin(), kFileKeys.end(), key) == kFileKeys.end()) {
            return propagate(failure(ErrorCode::ParseError, key, "unknown key", context));
        }
    }
    const auto version = document.find("schemaVersion");
    if (version == document.end() || !version->is_number_integer()) {
        return propagate(failure(ErrorCode::ParseError, "schemaVersion", "expected an integer", context));
    }
    if (*version != kPlayerSettingsSchemaVersion) {
        return propagate(failure(ErrorCode::Unsupported, "schemaVersion",
                                 fmt::format("{} is not {}", version->dump(), kPlayerSettingsSchemaVersion), context));
    }
    PlayerSettings settings;
    const auto fov = document.find("horizontalFovDeg");
    if (fov == document.end()) {
        return propagate(failure(ErrorCode::ParseError, "horizontalFovDeg", "missing", context));
    }
    if (!fov->is_null()) {
        auto degrees = number(document, "horizontalFovDeg", kMinPlayerFovDeg, kMaxPlayerFovDeg, context);
        if (!degrees) {
            return propagate(degrees.error());
        }
        settings.horizontalFovDeg = *degrees;
    }
    auto mouse = number(document, "mouseSensitivity", kMinPlayerSensitivity, kMaxPlayerSensitivity, context);
    if (!mouse) {
        return propagate(mouse.error());
    }
    settings.mouseSensitivity = *mouse;
    auto stick = number(document, "stickSensitivity", kMinPlayerSensitivity, kMaxPlayerSensitivity, context);
    if (!stick) {
        return propagate(stick.error());
    }
    settings.stickSensitivity = *stick;
    const auto invert = document.find("invertY");
    if (invert == document.end() || !invert->is_boolean()) {
        return propagate(failure(ErrorCode::ParseError, "invertY", "expected true or false", context));
    }
    settings.invertY = invert->get<bool>();
    const auto mode = document.find("displayMode");
    if (mode == document.end() || !mode->is_string()) {
        return propagate(failure(ErrorCode::ParseError, "displayMode", "expected a string", context));
    }
    if (*mode == "borderless") {
        settings.displayMode = DisplayMode::Borderless;
    } else if (*mode != "window") {
        return propagate(
            failure(ErrorCode::ValidationFailed, "displayMode", "expected one of \"window\", \"borderless\"", context));
    }
    auto scale = number(document, "textScale", kMinTextScale, kMaxTextScale, context);
    if (!scale) {
        return propagate(scale.error());
    }
    settings.textScale = *scale;
    return settings;
}

Expected<LoadedPlayerSettings> loadPlayerSettings(const std::filesystem::path& assetRoot,
                                                  const std::filesystem::path& userConfigDir) {
    const std::filesystem::path defaultsPath = assetRoot / "config" / core::pathFromUtf8(kPlayerSettingsFile);
    auto defaults = core::readJsonFile(defaultsPath);
    if (!defaults) {
        return propagate(defaults.error());
    }
    auto shipped = parsePlayerSettings(*defaults, core::pathToUtf8Lossy(defaultsPath));
    if (!shipped) {
        return propagate(shipped.error());
    }
    LoadedPlayerSettings loaded;
    loaded.settings = *shipped;
    loaded.belowSettings = *defaults;

    const std::filesystem::path userPath = userConfigDir / core::pathFromUtf8(kPlayerSettingsFile);
    std::error_code ignored;
    if (std::filesystem::exists(userPath, ignored)) {
        auto user = core::readJsonFile(userPath);
        if (!user) {
            loaded.issues.push_back(user.error());
        } else if (!user->is_object()) {
            loaded.issues.push_back(
                Error{ErrorCode::ParseError, "expected an object", core::pathToUtf8Lossy(userPath)});
        } else {
            Json document = core::mergeSettingsDocuments(*defaults, *user);
            if (auto merged = parsePlayerSettings(document, core::pathToUtf8Lossy(userPath))) {
                loaded.settings = *merged;
                loaded.belowSettings = std::move(document);
            } else {
                loaded.issues.push_back(merged.error());
            }
        }
    }

    const Json overlay =
        core::SettingsFile::loadBeside(userConfigDir).section(kPlayerSettingsSection, playerSettingKeys());
    if (!overlay.empty()) {
        const Json document = core::acceptedOverlay(
            loaded.belowSettings, overlay,
            [](const Json& candidate) { return parsePlayerSettings(candidate, "settings.json").has_value(); },
            &loaded.refusedKeys);
        if (auto merged = parsePlayerSettings(document, "settings.json")) {
            loaded.settings = *merged;
        }
    }
    return loaded;
}

} // namespace rv::app
