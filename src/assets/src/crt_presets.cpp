#include <rv/assets/crt_presets.hpp>

#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "json_util.hpp"

namespace rv::assets {
namespace {

constexpr std::array<std::string_view, kCrtPresetCount> kNames{"neutral", "consumer", "arcade", "monitor"};

/// The keys of a CrtEffect object, its fields and their ranges.
struct CrtField {
    std::string_view key;
    float CrtEffect::*field;
    float minValue;
    float maxValue;
};
constexpr std::array<CrtField, 7> kFields{{
    {"lineCount", &CrtEffect::lineCount, 16.0f, 2048.0f},
    {"scanlineStrength", &CrtEffect::scanlineStrength, 0.0f, 1.0f},
    {"maskTriadCount", &CrtEffect::maskTriadCount, 16.0f, 4096.0f},
    {"maskStrength", &CrtEffect::maskStrength, 0.0f, 1.0f},
    {"curvature", &CrtEffect::curvature, 0.0f, 0.5f},
    {"vignette", &CrtEffect::vignette, 0.0f, 1.0f},
    {"glassReflection", &CrtEffect::glassReflection, 0.0f, 1.0f},
}};

/// `base` with `overrides` merged in: members of objects present in both merged recursively, anything else replaced.
nlohmann::json merged(nlohmann::json base, const nlohmann::json& overrides) {
    if (!base.is_object() || !overrides.is_object()) {
        return overrides;
    }
    for (const auto& [key, value] : overrides.items()) {
        const auto existing = base.find(key);
        if (existing != base.end() && existing->is_object() && value.is_object()) {
            *existing = merged(std::move(*existing), value);
        } else {
            base[key] = value;
        }
    }
    return base;
}

} // namespace

std::string_view toString(CrtPreset preset) noexcept {
    const auto index = static_cast<std::size_t>(preset);
    return index < kNames.size() ? kNames[index] : std::string_view("consumer");
}

std::optional<CrtPreset> crtPresetFromName(std::string_view name) noexcept {
    for (const CrtPreset preset : kCrtPresets) {
        if (toString(preset) == name) {
            return preset;
        }
    }
    return std::nullopt;
}

const CrtPresets& builtInCrtPresets() noexcept {
    // A copy of assets/materials/crt_presets.json (unit-tested equal).
    static const CrtPresets presets = [] {
        CrtPresets values;
        values[CrtPreset::Neutral] = CrtEffect{.lineCount = 240.0f,
                                               .scanlineStrength = 0.0f,
                                               .maskTriadCount = 320.0f,
                                               .maskStrength = 0.0f,
                                               .curvature = 0.0f,
                                               .vignette = 0.0f,
                                               .glassReflection = 0.25f};
        values[CrtPreset::Consumer] = CrtEffect{};
        values[CrtPreset::Arcade] = CrtEffect{.lineCount = 240.0f,
                                              .scanlineStrength = 0.5f,
                                              .maskTriadCount = 240.0f,
                                              .maskStrength = 0.35f,
                                              .curvature = 0.09f,
                                              .vignette = 0.3f,
                                              .glassReflection = 0.2f};
        values[CrtPreset::Monitor] = CrtEffect{.lineCount = 480.0f,
                                               .scanlineStrength = 0.12f,
                                               .maskTriadCount = 640.0f,
                                               .maskStrength = 0.1f,
                                               .curvature = 0.02f,
                                               .vignette = 0.12f,
                                               .glassReflection = 0.25f};
        return values;
    }();
    return presets;
}

Expected<CrtEffect> parseCrtEffect(const nlohmann::json& object, const CrtEffect& base, bool requireAll,
                                   std::string_view context) {
    if (auto status = detail::requireObject(object, context); !status) {
        return propagate(status.error());
    }
    if (auto status = detail::rejectUnknownKeys(object,
                                                {"lineCount", "scanlineStrength", "maskTriadCount", "maskStrength",
                                                 "curvature", "vignette", "glassReflection"},
                                                context);
        !status) {
        return propagate(status.error());
    }
    CrtEffect effect = base;
    for (const CrtField& field : kFields) {
        const auto it = object.find(field.key);
        if (it == object.end()) {
            if (requireAll) {
                return makeError(ErrorCode::ParseError, "missing \"" + std::string(field.key) + "\"",
                                 detail::pointerChild(context, field.key));
            }
            continue;
        }
        auto value =
            detail::readNumberInRange(*it, field.minValue, field.maxValue, detail::pointerChild(context, field.key));
        if (!value) {
            return propagate(value.error());
        }
        effect.*field.field = *value;
    }
    return effect;
}

Expected<CrtPresets> parseCrtPresets(const nlohmann::json& document, std::string_view context) {
    const std::string root = std::string(context) + ":";
    if (auto status = detail::requireObject(document, root + "/"); !status) {
        return propagate(status.error());
    }
    if (auto status = detail::rejectUnknownKeys(document, {"schemaVersion", "presets"}, root); !status) {
        return propagate(status.error());
    }
    const std::string versionContext = detail::pointerChild(root, "schemaVersion");
    const auto version = document.find("schemaVersion");
    if (version == document.end()) {
        return makeError(ErrorCode::ParseError, "missing \"schemaVersion\"", versionContext);
    }
    auto number = detail::readInteger(*version, versionContext);
    if (!number) {
        return propagate(number.error());
    }
    if (*number != kCrtPresetsSchemaVersion) {
        return makeError(ErrorCode::Unsupported, "unsupported schemaVersion " + std::to_string(*number),
                         versionContext);
    }
    const std::string presetsContext = detail::pointerChild(root, "presets");
    const auto presets = document.find("presets");
    if (presets == document.end()) {
        return makeError(ErrorCode::ParseError, "missing \"presets\"", presetsContext);
    }
    if (auto status = detail::requireObject(*presets, presetsContext); !status) {
        return propagate(status.error());
    }
    if (auto status =
            detail::rejectUnknownKeys(*presets, {kNames[0], kNames[1], kNames[2], kNames[3]}, presetsContext);
        !status) {
        return propagate(status.error());
    }
    CrtPresets values;
    for (const CrtPreset preset : kCrtPresets) {
        const std::string where = detail::pointerChild(presetsContext, toString(preset));
        const auto object = presets->find(toString(preset));
        if (object == presets->end()) {
            return makeError(ErrorCode::ParseError, "missing \"" + std::string(toString(preset)) + "\"", where);
        }
        auto effect = parseCrtEffect(*object, CrtEffect{}, true, where);
        if (!effect) {
            return propagate(effect.error());
        }
        values[preset] = *effect;
    }
    return values;
}

LoadedCrtPresets loadCrtPresets(const std::filesystem::path& assetRoot, const std::filesystem::path& userConfigDir) {
    LoadedCrtPresets loaded;
    const std::filesystem::path shippedPath = assetRoot / "materials" / core::pathFromUtf8(kCrtPresetsFile);
    auto shipped = core::readJsonFile(shippedPath);
    if (!shipped) {
        loaded.issues.push_back(shipped.error());
        return loaded;
    }
    auto values = parseCrtPresets(*shipped, core::pathToUtf8Lossy(shippedPath));
    if (!values) {
        loaded.issues.push_back(values.error());
        return loaded;
    }
    loaded.presets = *values;
    loaded.source = "shipped";

    const std::filesystem::path userPath = userConfigDir / core::pathFromUtf8(kCrtPresetsFile);
    std::error_code ignored;
    if (userConfigDir.empty() || !std::filesystem::exists(userPath, ignored)) {
        return loaded;
    }
    auto user = core::readJsonFile(userPath);
    if (!user) {
        loaded.issues.push_back(user.error());
        return loaded;
    }
    if (!user->is_object()) {
        loaded.issues.push_back(Error{ErrorCode::ParseError, "expected an object", core::pathToUtf8Lossy(userPath)});
        return loaded;
    }
    auto over = parseCrtPresets(merged(*shipped, *user), core::pathToUtf8Lossy(userPath));
    if (!over) {
        loaded.issues.push_back(over.error());
        return loaded;
    }
    loaded.presets = *over;
    loaded.source = "user";
    return loaded;
}

} // namespace rv::assets
