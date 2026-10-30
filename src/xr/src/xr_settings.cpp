#include <rv/xr/xr_settings.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::xr {
namespace {

using Json = nlohmann::json;

/// Reads one object of the document, every key required and none unknown; the first problem is kept.
class Reader {
public:
    Reader(const Json& object, std::string pointer, std::string_view context, std::optional<Error>& error,
           std::initializer_list<std::string_view> known)
        : m_object(object), m_pointer(std::move(pointer)), m_context(context), m_error(error) {
        if (!m_object.is_object()) {
            fail(m_pointer, "expected an object");
            return;
        }
        for (const auto& [key, value] : m_object.items()) {
            if (std::find(known.begin(), known.end(), key) == known.end()) {
                fail(m_pointer + "/" + key, "unknown key");
            }
        }
    }

    [[nodiscard]] const Json* value(std::string_view key) {
        if (!m_object.is_object() || !m_object.contains(std::string(key))) {
            fail(pointer(key), "missing");
            return nullptr;
        }
        return &m_object.at(std::string(key));
    }
    [[nodiscard]] std::string pointer(std::string_view key) const { return m_pointer + "/" + std::string(key); }

    std::optional<double> number(std::string_view key, double low, double high) {
        const Json* v = value(key);
        if (v == nullptr) {
            return std::nullopt;
        }
        if (!v->is_number()) {
            fail(pointer(key), "expected a number");
            return std::nullopt;
        }
        const double number = v->get<double>();
        if (!std::isfinite(number) || number < low || number > high) {
            fail(pointer(key), fmt::format("expected a number from {:g} to {:g}", low, high));
            return std::nullopt;
        }
        return number;
    }
    /// A string among `names`: its index.
    template <std::size_t N>
    std::optional<std::size_t> name(std::string_view key, const std::array<std::string_view, N>& names) {
        const Json* v = value(key);
        if (v == nullptr) {
            return std::nullopt;
        }
        if (v->is_string()) {
            const std::string text = v->get<std::string>();
            for (std::size_t index = 0; index < N; ++index) {
                if (text == names[index]) {
                    return index;
                }
            }
        }
        std::string expected;
        for (const std::string_view known : names) {
            expected += (expected.empty() ? "\"" : ", \"") + std::string(known) + "\"";
        }
        fail(pointer(key), "expected one of " + expected);
        return std::nullopt;
    }

    void fail(const std::string& at, std::string_view message) {
        if (!m_error) {
            m_error = Error{ErrorCode::ValidationFailed, at + ": " + std::string(message), std::string(m_context)};
        }
    }

private:
    const Json& m_object;
    std::string m_pointer;
    std::string_view m_context;
    std::optional<Error>& m_error;
};

constexpr std::array<std::string_view, 2> kModes = {"desktop", "vr"};
constexpr std::array<std::string_view, 2> kPostures = {"standing", "seated"};
constexpr std::array<std::string_view, 2> kTurns = {"snap", "smooth"};
constexpr std::array<std::string_view, 2> kMoveReferences = {"head", "hand"};

Json merge(Json base, const Json& overrides) {
    if (!base.is_object() || !overrides.is_object()) {
        return overrides;
    }
    for (const auto& [key, value] : overrides.items()) {
        const auto existing = base.find(key);
        if (existing != base.end() && existing->is_object() && value.is_object()) {
            *existing = merge(std::move(*existing), value);
        } else {
            base[key] = value;
        }
    }
    return base;
}

} // namespace

Expected<XrSettings> parseXrSettings(const nlohmann::json& document, std::string_view context) {
    std::optional<Error> error;
    XrSettings settings;
    Reader root(document, "", context, error,
                {"schemaVersion", "display", "renderScale", "refreshRate", "posture", "comfort"});
    if (const std::optional<double> version = root.number("schemaVersion", 1, 1000);
        version && *version != kXrSettingsSchemaVersion) {
        root.fail("/schemaVersion", fmt::format("expected {}", kXrSettingsSchemaVersion));
    }
    if (const Json* display = root.value("display")) {
        Reader reader(*display, "/display", context, error, {"mode"});
        if (const auto mode = reader.name("mode", kModes)) {
            settings.vr = *mode == 1;
        }
    }
    if (const auto scale = root.number("renderScale", XrSettings::kMinRenderScale, XrSettings::kMaxRenderScale)) {
        settings.renderScale = *scale;
    }
    if (const auto rate = root.number("refreshRate", 0.0, XrSettings::kMaxRefreshRate)) {
        settings.refreshRate = static_cast<float>(*rate);
    }
    if (const auto posture = root.name("posture", kPostures)) {
        settings.posture = *posture == 1 ? Posture::Seated : Posture::Standing;
    }
    if (const Json* comfort = root.value("comfort")) {
        Reader reader(*comfort, "/comfort", context, error,
                      {"turn", "snapDegrees", "smoothDegreesPerSecond", "vignette", "moveRelativeTo"});
        if (const auto turn = reader.name("turn", kTurns)) {
            settings.comfort.turn = *turn == 1 ? TurnMode::Smooth : TurnMode::Snap;
        }
        if (const auto degrees = reader.number("snapDegrees", 1.0, 180.0)) {
            settings.comfort.snapDegrees = static_cast<float>(*degrees);
        }
        if (const auto rate = reader.number("smoothDegreesPerSecond", 1.0, 720.0)) {
            settings.comfort.smoothDegreesPerSecond = static_cast<float>(*rate);
        }
        if (const auto vignette = reader.number("vignette", 0.0, 1.0)) {
            settings.comfort.vignette = static_cast<float>(*vignette);
        }
        if (const auto reference = reader.name("moveRelativeTo", kMoveReferences)) {
            settings.comfort.moveWithHand = *reference == 1;
        }
    }
    if (error) {
        return propagate(*error);
    }
    return settings;
}

Expected<LoadedXrSettings> loadXrSettings(const std::filesystem::path& assetRoot,
                                          const std::filesystem::path& userConfigDir) {
    const std::filesystem::path defaultsPath = assetRoot / "config" / core::pathFromUtf8(kXrSettingsFile);
    auto defaults = core::readJsonFile(defaultsPath);
    if (!defaults) {
        return propagate(defaults.error());
    }
    auto shipped = parseXrSettings(*defaults, core::pathToUtf8Lossy(defaultsPath));
    if (!shipped) {
        return propagate(shipped.error());
    }
    LoadedXrSettings loaded;
    loaded.settings = *shipped;

    const std::filesystem::path userPath = userConfigDir / core::pathFromUtf8(kXrSettingsFile);
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
    auto merged = parseXrSettings(merge(*defaults, *user), core::pathToUtf8Lossy(userPath));
    if (!merged) {
        loaded.issues.push_back(merged.error());
        return loaded;
    }
    loaded.settings = *merged;
    return loaded;
}

} // namespace rv::xr
