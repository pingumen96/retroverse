#include <rv/app/video_settings.hpp>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace rv::app {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, 4> kCrtPresets = {"neutral", "consumer", "arcade", "monitor"};

/// Reads one object of the document, refusing unknown keys; the first problem is kept and the reads after it are
/// harmless (a missing value reads as its default).
class Reader {
public:
    Reader(const Json& object, std::string pointer, std::string_view context, std::optional<Error>& error,
           std::initializer_list<std::string_view> known)
        : m_object(object), m_pointer(std::move(pointer)), m_context(context), m_error(error) {
        if (!m_object.is_object()) {
            fail(ErrorCode::ParseError, m_pointer, "expected an object");
            return;
        }
        for (const auto& [key, value] : m_object.items()) {
            if (std::find(known.begin(), known.end(), key) == known.end()) {
                fail(ErrorCode::ParseError, m_pointer + "/" + key, "unknown key");
            }
        }
    }

    [[nodiscard]] bool has(std::string_view key) const {
        return m_object.is_object() && m_object.contains(std::string(key));
    }
    [[nodiscard]] const Json* value(std::string_view key, bool required) {
        if (!has(key)) {
            if (required) {
                fail(ErrorCode::ParseError, pointer(key), "missing");
            }
            return nullptr;
        }
        return &m_object.at(std::string(key));
    }
    [[nodiscard]] std::string pointer(std::string_view key) const { return m_pointer + "/" + std::string(key); }

    std::optional<bool> boolean(std::string_view key, bool required) {
        const Json* v = value(key, required);
        if (v == nullptr) {
            return std::nullopt;
        }
        if (!v->is_boolean()) {
            fail(ErrorCode::ParseError, pointer(key), "expected true or false");
            return std::nullopt;
        }
        return v->get<bool>();
    }
    std::optional<double> number(std::string_view key, bool required, double low, double high) {
        const Json* v = value(key, required);
        if (v == nullptr) {
            return std::nullopt;
        }
        if (!v->is_number()) {
            fail(ErrorCode::ParseError, pointer(key), "expected a number");
            return std::nullopt;
        }
        const double number = v->get<double>();
        if (!std::isfinite(number) || number < low || number > high) {
            fail(ErrorCode::ValidationFailed, pointer(key), fmt::format("expected a number from {:g} to {:g}", low, high));
            return std::nullopt;
        }
        return number;
    }
    std::optional<int> integer(std::string_view key, bool required, int low, int high) {
        const Json* v = value(key, required);
        if (v == nullptr) {
            return std::nullopt;
        }
        if (!v->is_number_integer()) {
            fail(ErrorCode::ParseError, pointer(key), "expected an integer");
            return std::nullopt;
        }
        const auto number = v->get<std::int64_t>();
        if (number < low || number > high) {
            fail(ErrorCode::ValidationFailed, pointer(key), fmt::format("expected an integer from {} to {}", low, high));
            return std::nullopt;
        }
        return static_cast<int>(number);
    }
    std::optional<std::string> string(std::string_view key, bool required) {
        const Json* v = value(key, required);
        if (v == nullptr) {
            return std::nullopt;
        }
        if (!v->is_string()) {
            fail(ErrorCode::ParseError, pointer(key), "expected a string");
            return std::nullopt;
        }
        std::string text = v->get<std::string>();
        if (!core::isValidUtf8(text)) {
            fail(ErrorCode::ValidationFailed, pointer(key), "not valid UTF-8");
            return std::nullopt;
        }
        return text;
    }
    /// A string among `names` (their order the message's).
    template <class Names>
    std::optional<std::string> name(std::string_view key, bool required, const Names& names) {
        std::optional<std::string> text = string(key, required);
        if (text && std::find(std::begin(names), std::end(names), *text) == std::end(names)) {
            std::string expected;
            for (const auto& known : names) {
                expected += (expected.empty() ? "\"" : ", \"") + std::string(known) + "\"";
            }
            fail(ErrorCode::ValidationFailed, pointer(key), "expected one of " + expected);
            return std::nullopt;
        }
        return text;
    }

    void fail(ErrorCode code, const std::string& pointer, std::string_view message) {
        if (!m_error) {
            m_error = Error{code, pointer + ": " + std::string(message), std::string(m_context)};
        }
    }

private:
    const Json& m_object;
    std::string m_pointer;
    std::string_view m_context;
    std::optional<Error>& m_error;
};

constexpr std::array<std::string_view, 5> kPlaylistNames = {"shuffle", "sequential", "favorites", "recent",
                                                            "explicit"};

ScreenOverrides readScreen(const Json& object, const std::string& pointer, std::string_view context,
                           std::optional<Error>& error, bool required, bool room) {
    ScreenOverrides screen;
    Reader reader(object, pointer, context, error,
                  room ? std::initializer_list<std::string_view>{"enabled", "playlist", "previewDuration",
                                                                 "showOnFocus", "volume", "crtPreset", "games"}
                       : std::initializer_list<std::string_view>{"enabled", "playlist", "previewDuration",
                                                                 "showOnFocus", "volume", "crtPreset"});
    screen.enabled = reader.boolean("enabled", required);
    if (auto mode = reader.name("playlist", required, kPlaylistNames)) {
        screen.playlist = parsePlaylistMode(*mode);
    }
    screen.previewSeconds = reader.number("previewDuration", required, kMinPreviewSeconds, kMaxPreviewSeconds);
    screen.showOnFocus = reader.boolean("showOnFocus", required);
    screen.volume = reader.number("volume", required, 0.0, 1.0);
    if (const Json* preset = reader.value("crtPreset", required)) {
        if (preset->is_string() && preset->get<std::string>().empty()) {
            screen.crtPreset = std::string();
        } else {
            screen.crtPreset = reader.name("crtPreset", required, kCrtPresets);
        }
    }
    if (room) {
        if (const Json* games = reader.value("games", false)) {
            if (!games->is_array()) {
                reader.fail(ErrorCode::ParseError, reader.pointer("games"), "expected an array of GameIds");
            } else {
                std::vector<std::string> ids;
                for (std::size_t index = 0; index < games->size(); ++index) {
                    const Json& id = (*games)[index];
                    if (!id.is_string() || id.get<std::string>().empty() || !core::isValidUtf8(id.get<std::string>())) {
                        reader.fail(ErrorCode::ParseError, fmt::format("{}/{}", reader.pointer("games"), index),
                                    "expected a GameId");
                        continue;
                    }
                    ids.push_back(id.get<std::string>());
                }
                screen.games = std::move(ids);
            }
        }
    }
    return screen;
}

ScreenSettings resolved(ScreenSettings base, const ScreenOverrides& overrides) {
    if (overrides.enabled) {
        base.enabled = *overrides.enabled;
    }
    if (overrides.playlist) {
        base.playlist = *overrides.playlist;
    }
    if (overrides.previewSeconds) {
        base.previewSeconds = *overrides.previewSeconds;
    }
    if (overrides.showOnFocus) {
        base.showOnFocus = *overrides.showOnFocus;
    }
    if (overrides.volume) {
        base.volume = *overrides.volume;
    }
    if (overrides.crtPreset) {
        base.crtPreset = *overrides.crtPreset;
    }
    if (overrides.games) {
        base.games = *overrides.games;
    }
    return base;
}

} // namespace

std::string_view toString(ScreenKind kind) noexcept {
    switch (kind) {
    case ScreenKind::Hero:
        return "hero";
    case ScreenKind::Secondary:
        return "secondary";
    case ScreenKind::Cabinet:
        return "cabinet";
    }
    return "unknown";
}

std::string_view toString(CrtEffect effect) noexcept {
    switch (effect) {
    case CrtEffect::Off:
        return "off";
    case CrtEffect::Light:
        return "light";
    case CrtEffect::Vintage:
        return "vintage";
    }
    return "unknown";
}

std::string_view toString(AudioMode mode) noexcept {
    switch (mode) {
    case AudioMode::NearestFocused:
        return "nearest-focused";
    case AudioMode::Off:
        return "off";
    }
    return "unknown";
}

ScreenSettings VideoSettings::screen(ScreenKind kind, std::string_view roomId) const {
    const ScreenSettings& base = kind == ScreenKind::Hero ? hero : kind == ScreenKind::Secondary ? secondary : cabinet;
    const auto room = rooms.find(roomId);
    if (room == rooms.end()) {
        return base;
    }
    const std::optional<ScreenOverrides>& overrides = kind == ScreenKind::Hero        ? room->second.hero
                                                      : kind == ScreenKind::Secondary ? room->second.secondary
                                                                                      : room->second.cabinet;
    return overrides ? resolved(base, *overrides) : base;
}

Expected<VideoSettings> parseVideoSettings(const Json& document, std::string_view context) {
    std::optional<Error> error;
    Reader reader(document, "", context, error,
                  {"schemaVersion", "enabled", "maxDecoders", "maxRows", "viewerMaxRows", "proximityMeters",
                   "crtEffect", "audio", "hero", "secondary", "cabinet", "rooms"});
    if (error) {
        return propagate(*error);
    }
    const std::optional<int> version = reader.integer("schemaVersion", true, 0, 1'000'000);
    if (error) {
        return propagate(*error);
    }
    if (version != kVideoSettingsSchemaVersion) {
        return makeError(ErrorCode::Unsupported,
                         fmt::format("/schemaVersion: {} is not {}", *version, kVideoSettingsSchemaVersion),
                         std::string(context));
    }
    VideoSettings settings;
    settings.enabled = reader.boolean("enabled", true).value_or(true);
    settings.maxDecoders = reader.integer("maxDecoders", true, 1, 2).value_or(2);
    settings.maxRows = reader.integer("maxRows", true, kMinVideoRows, kMaxVideoRows).value_or(480);
    settings.viewerMaxRows = reader.integer("viewerMaxRows", true, kMinVideoRows, kMaxVideoRows).value_or(720);
    settings.proximityMeters = reader.number("proximityMeters", true, 0.0, kMaxProximityMeters).value_or(12.0);
    constexpr std::array<std::string_view, 3> kEffects = {"off", "light", "vintage"};
    if (const auto effect = reader.name("crtEffect", true, kEffects)) {
        settings.crtEffect = *effect == "off" ? CrtEffect::Off : *effect == "vintage" ? CrtEffect::Vintage : CrtEffect::Light;
    }
    if (const Json* audio = reader.value("audio", true)) {
        Reader audioReader(*audio, "/audio", context, error,
                           {"mode", "master", "video", "effects", "ambience", "muted"});
        constexpr std::array<std::string_view, 2> kModes = {"nearest-focused", "off"};
        if (const auto mode = audioReader.name("mode", true, kModes)) {
            settings.audio.mode = *mode == "off" ? AudioMode::Off : AudioMode::NearestFocused;
        }
        settings.audio.master = audioReader.number("master", true, 0.0, 1.0).value_or(1.0);
        settings.audio.video = audioReader.number("video", true, 0.0, 1.0).value_or(1.0);
        settings.audio.effects = audioReader.number("effects", true, 0.0, 1.0).value_or(1.0);
        settings.audio.ambience = audioReader.number("ambience", true, 0.0, 1.0).value_or(1.0);
        settings.audio.muted = audioReader.boolean("muted", true).value_or(false);
    }
    const std::pair<std::string_view, ScreenSettings*> kinds[] = {
        {"hero", &settings.hero}, {"secondary", &settings.secondary}, {"cabinet", &settings.cabinet}};
    for (const auto& [key, target] : kinds) {
        if (const Json* object = reader.value(key, true)) {
            *target = resolved(ScreenSettings{}, readScreen(*object, "/" + std::string(key), context, error, true, false));
        }
    }
    if (const Json* rooms = reader.value("rooms", false)) {
        if (!rooms->is_object()) {
            reader.fail(ErrorCode::ParseError, "/rooms", "expected an object of room ids");
        } else {
            for (const auto& [roomId, room] : rooms->items()) {
                const std::string pointer = "/rooms/" + roomId;
                if (roomId.empty() || !core::isValidUtf8(roomId)) {
                    reader.fail(ErrorCode::ValidationFailed, pointer, "expected a room id");
                    continue;
                }
                Reader roomReader(room, pointer, context, error, {"hero", "secondary", "cabinet"});
                RoomVideoOverrides overrides;
                const std::pair<std::string_view, std::optional<ScreenOverrides>*> roomKinds[] = {
                    {"hero", &overrides.hero}, {"secondary", &overrides.secondary}, {"cabinet", &overrides.cabinet}};
                for (const auto& [key, target] : roomKinds) {
                    if (const Json* object = roomReader.value(key, false)) {
                        *target = readScreen(*object, pointer + "/" + std::string(key), context, error, false, true);
                    }
                }
                settings.rooms.emplace(roomId, std::move(overrides));
            }
        }
    }
    if (error) {
        return propagate(*error);
    }
    return settings;
}

Json mergeVideoDocuments(Json base, const Json& overrides) {
    if (!base.is_object() || !overrides.is_object()) {
        return overrides;
    }
    for (const auto& [key, value] : overrides.items()) {
        const auto existing = base.find(key);
        if (existing != base.end() && existing->is_object() && value.is_object()) {
            *existing = mergeVideoDocuments(std::move(*existing), value);
        } else {
            base[key] = value;
        }
    }
    return base;
}

Expected<LoadedVideoSettings> loadVideoSettings(const std::filesystem::path& assetRoot,
                                                const std::filesystem::path& userConfigDir) {
    const std::filesystem::path defaultsPath = assetRoot / "config" / core::pathFromUtf8(kVideoSettingsFile);
    auto defaults = core::readJsonFile(defaultsPath);
    if (!defaults) {
        return propagate(defaults.error());
    }
    auto shipped = parseVideoSettings(*defaults, core::pathToUtf8Lossy(defaultsPath));
    if (!shipped) {
        return propagate(shipped.error());
    }
    LoadedVideoSettings loaded;
    loaded.settings = std::move(*shipped);

    const std::filesystem::path userPath = userConfigDir / core::pathFromUtf8(kVideoSettingsFile);
    std::error_code ignored;
    if (!std::filesystem::exists(userPath, ignored)) {
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
    auto merged = parseVideoSettings(mergeVideoDocuments(*defaults, *user), core::pathToUtf8Lossy(userPath));
    if (!merged) {
        loaded.issues.push_back(merged.error());
        return loaded;
    }
    loaded.settings = std::move(*merged);
    return loaded;
}

} // namespace rv::app
