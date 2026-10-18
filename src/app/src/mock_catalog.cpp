#include <rv/app/mock_catalog.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <set>
#include <span>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/json.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

namespace rv::app {
namespace {

using Json = nlohmann::json;

constexpr int kMaxYear = 9999;

/// Where a value sits: the catalog file and a JSON pointer (RFC 6901).
class Location {
public:
    explicit Location(std::string file) : m_file(std::move(file)) {}

    [[nodiscard]] Location child(std::string_view key) const {
        Location location(*this);
        location.m_pointer.push_back('/');
        for (const char c : key) {
            if (c == '~') {
                location.m_pointer += "~0";
            } else if (c == '/') {
                location.m_pointer += "~1";
            } else {
                location.m_pointer.push_back(c);
            }
        }
        return location;
    }
    [[nodiscard]] Location child(std::size_t index) const {
        Location location(*this);
        location.m_pointer += "/" + std::to_string(index);
        return location;
    }
    /// "<file>:<pointer>", or the file alone at the document's root.
    [[nodiscard]] std::string context() const { return m_pointer.empty() ? m_file : m_file + ":" + m_pointer; }

private:
    std::string m_file;
    std::string m_pointer;
};

[[nodiscard]] tl::unexpected<Error> fail(ErrorCode code, std::string message, const Location& at) {
    return makeError(code, std::move(message), at.context());
}

/// Every key of `object` is one of `known`.
Status checkKeys(const Json& object, std::span<const std::string_view> known, const Location& at) {
    for (const auto& [key, value] : object.items()) {
        if (std::find(known.begin(), known.end(), key) == known.end()) {
            return fail(ErrorCode::ParseError, fmt::format("unknown field \"{}\"", key), at.child(key));
        }
    }
    return ok();
}

/// The object at `at`.
Status requireObject(const Json& value, const Location& at) {
    if (!value.is_object()) {
        return fail(ErrorCode::ParseError, "expected an object", at);
    }
    return ok();
}

/// A string field: required (missing: ParseError) or optional (missing: `fallback`), valid UTF-8.
Expected<std::string> stringField(const Json& object, std::string_view key, const Location& at, bool required,
                                  std::string fallback = {}) {
    const auto it = object.find(key);
    if (it == object.end()) {
        if (required) {
            return fail(ErrorCode::ParseError, fmt::format("missing required field \"{}\"", key), at);
        }
        return fallback;
    }
    if (!it->is_string()) {
        return fail(ErrorCode::ParseError, "expected a string", at.child(key));
    }
    std::string text = it->get<std::string>();
    if (!core::isValidUtf8(text)) {
        return fail(ErrorCode::ValidationFailed, "not valid UTF-8", at.child(key));
    }
    return text;
}

Expected<MockLaunch> parseLaunch(const Json& value, const std::filesystem::path& folder, std::string title,
                                 const Location& at) {
    if (auto object = requireObject(value, at); !object) {
        return propagate(object.error());
    }
    static constexpr std::array<std::string_view, 4> kKnown{"profile", "rom", "system", "variables"};
    if (auto keys = checkKeys(value, kKnown, at); !keys) {
        return propagate(keys.error());
    }
    MockLaunch launch;
    auto profile = stringField(value, "profile", at, true);
    if (!profile) {
        return propagate(profile.error());
    }
    if (profile->empty()) {
        return fail(ErrorCode::ValidationFailed, "an empty profile id", at.child("profile"));
    }
    launch.profile = std::move(*profile);
    auto rom = stringField(value, "rom", at, true);
    if (!rom) {
        return propagate(rom.error());
    }
    auto romPath = core::tryPathFromUtf8(*rom);
    if (!romPath || rom->empty()) {
        return fail(ErrorCode::ValidationFailed, "not a usable path", at.child("rom"));
    }
    launch.target.rom = (romPath->is_absolute() ? *romPath : folder / *romPath).lexically_normal();
    auto system = stringField(value, "system", at, false);
    if (!system) {
        return propagate(system.error());
    }
    launch.target.system = std::move(*system);
    if (const auto variables = value.find("variables"); variables != value.end()) {
        const Location where = at.child("variables");
        if (!variables->is_object()) {
            return fail(ErrorCode::ParseError, "expected an object of strings", where);
        }
        for (const auto& [name, text] : variables->items()) {
            if (!text.is_string()) {
                return fail(ErrorCode::ParseError, "expected a string", where.child(name));
            }
            launch.target.variables.emplace(name, text.get<std::string>());
        }
    }
    launch.target.title = std::move(title);
    return launch;
}

/// A path field of "media": absolute, or relative to the catalog's folder.
Expected<std::filesystem::path> mediaField(const Json& object, std::string_view key, const std::filesystem::path& folder,
                                           const Location& at) {
    auto text = stringField(object, key, at, false);
    if (!text) {
        return propagate(text.error());
    }
    if (text->empty()) {
        return std::filesystem::path();
    }
    auto path = core::tryPathFromUtf8(*text);
    if (!path) {
        return fail(ErrorCode::ValidationFailed, "not a usable path", at.child(key));
    }
    return (path->is_absolute() ? *path : folder / *path).lexically_normal();
}

/// "media": what the hero CRT shows of the game (I3 step 11).
Expected<ScreenMedia> parseMedia(const Json& value, const std::filesystem::path& folder, const Location& at) {
    if (auto object = requireObject(value, at); !object) {
        return propagate(object.error());
    }
    static constexpr std::array<std::string_view, 4> kKnown{"video", "screenshot", "titlescreen", "cover"};
    if (auto keys = checkKeys(value, kKnown, at); !keys) {
        return propagate(keys.error());
    }
    ScreenMedia media;
    const std::pair<std::string_view, std::filesystem::path ScreenMedia::*> fields[] = {
        {"video", &ScreenMedia::video},
        {"screenshot", &ScreenMedia::screenshot},
        {"titlescreen", &ScreenMedia::titleScreen},
        {"cover", &ScreenMedia::cover},
    };
    for (const auto& [key, member] : fields) {
        auto path = mediaField(value, key, folder, at);
        if (!path) {
            return propagate(path.error());
        }
        media.*member = std::move(*path);
    }
    return media;
}

Expected<MockGame> parseGame(const Json& value, const std::filesystem::path& folder, const Location& at) {
    if (auto object = requireObject(value, at); !object) {
        return propagate(object.error());
    }
    static constexpr std::array<std::string_view, 13> kKnown{
        "id",      "title",       "platform", "year",   "developer", "publisher", "genre",
        "players", "description", "state",    "launch", "media",     "lastPlayed"};
    if (auto keys = checkKeys(value, kKnown, at); !keys) {
        return propagate(keys.error());
    }
    MockGame game;
    ui::CatalogEntry& entry = game.entry;
    using TextMember = decltype(&ui::CatalogEntry::id);
    const std::pair<std::string_view, TextMember> texts[] = {
        {"id", &ui::CatalogEntry::id},
        {"title", &ui::CatalogEntry::title},
        {"platform", &ui::CatalogEntry::platform},
        {"developer", &ui::CatalogEntry::developer},
        {"publisher", &ui::CatalogEntry::publisher},
        {"genre", &ui::CatalogEntry::genre},
        {"players", &ui::CatalogEntry::players},
        {"description", &ui::CatalogEntry::description},
        {"state", &ui::CatalogEntry::state},
    };
    for (const auto& [key, member] : texts) {
        auto text = stringField(value, key, at, key == "id" || key == "title");
        if (!text) {
            return propagate(text.error());
        }
        entry.*member = std::move(*text);
    }
    if (!ui::isValidEntryId(entry.id)) {
        return fail(ErrorCode::ValidationFailed,
                    fmt::format("\"{}\" is not a catalog id: 1 to 64 ASCII letters, digits, '-' or '_'", entry.id),
                    at.child("id"));
    }
    if (entry.title.empty()) {
        return fail(ErrorCode::ValidationFailed, "an empty title", at.child("title"));
    }
    if (const auto year = value.find("year"); year != value.end()) {
        if (!year->is_number_integer() || year->get<std::int64_t>() < 0 || year->get<std::int64_t>() > kMaxYear) {
            return fail(ErrorCode::ValidationFailed, fmt::format("expected a year from 0 to {}", kMaxYear),
                        at.child("year"));
        }
        entry.year = static_cast<int>(year->get<std::int64_t>());
    }
    if (const auto launch = value.find("launch"); launch != value.end()) {
        auto parsed = parseLaunch(*launch, folder, entry.title, at.child("launch"));
        if (!parsed) {
            return propagate(parsed.error());
        }
        game.launch = std::move(*parsed);
    }
    if (const auto media = value.find("media"); media != value.end()) {
        auto parsed = parseMedia(*media, folder, at.child("media"));
        if (!parsed) {
            return propagate(parsed.error());
        }
        game.media = std::move(*parsed);
    }
    if (value.contains("lastPlayed")) {
        auto text = stringField(value, "lastPlayed", at, true);
        if (!text) {
            return propagate(text.error());
        }
        auto played = core::parseUtcIso8601(*text);
        if (!played) {
            return fail(ErrorCode::ValidationFailed, "expected a UTC ISO-8601 time", at.child("lastPlayed"));
        }
        game.lastPlayed = std::chrono::duration_cast<std::chrono::seconds>(played->time_since_epoch()).count();
    }
    return game;
}

} // namespace

const MockGame* MockCatalog::find(std::string_view id) const noexcept {
    const auto it = std::find_if(games.begin(), games.end(), [&](const MockGame& game) { return game.entry.id == id; });
    return it == games.end() ? nullptr : &*it;
}

std::vector<ui::CatalogEntry> MockCatalog::entries() const {
    std::vector<ui::CatalogEntry> list;
    list.reserve(games.size());
    for (const MockGame& game : games) {
        list.push_back(game.entry);
    }
    return list;
}

std::vector<ScreenGame> MockCatalog::screenGames() const {
    std::vector<ScreenGame> list;
    list.reserve(games.size());
    for (const MockGame& game : games) {
        list.push_back(ScreenGame{game.entry.id, game.media, game.lastPlayed});
    }
    return list;
}

Expected<MockCatalog> parseMockCatalog(const nlohmann::json& document, const std::filesystem::path& file) {
    const std::string fileName = core::pathToUtf8Lossy(file);
    const Location root(fileName);
    if (auto object = requireObject(document, root); !object) {
        return propagate(object.error());
    }
    const auto version = document.find("schemaVersion");
    if (version == document.end() || !version->is_number_integer()) {
        return fail(ErrorCode::ParseError, "missing required integer field \"schemaVersion\"", root);
    }
    if (version->get<std::int64_t>() != kMockCatalogSchemaVersion) {
        return fail(ErrorCode::Unsupported,
                    fmt::format("schemaVersion {} is not supported (expected {})", version->dump(),
                                kMockCatalogSchemaVersion),
                    root.child("schemaVersion"));
    }
    static constexpr std::array<std::string_view, 3> kKnown{"schemaVersion", "profiles", "games"};
    if (auto keys = checkKeys(document, kKnown, root); !keys) {
        return propagate(keys.error());
    }
    MockCatalog catalog;
    catalog.file = file;
    const auto games = document.find("games");
    if (games == document.end()) {
        return fail(ErrorCode::ParseError, "missing required field \"games\"", root);
    }
    if (!games->is_array()) {
        return fail(ErrorCode::ParseError, "expected an array of games", root.child("games"));
    }
    // Relative ROM paths are relative to the catalog file's folder, made absolute (the launch plan needs absolute ROMs).
    std::error_code absoluteError;
    std::filesystem::path folder = std::filesystem::absolute(file.parent_path(), absoluteError);
    if (absoluteError) {
        folder = file.parent_path();
    }
    std::set<std::string, std::less<>> ids;
    for (std::size_t index = 0; index < games->size(); ++index) {
        const Location at = root.child("games").child(index);
        auto game = parseGame((*games)[index], folder, at);
        if (!game) {
            return propagate(game.error());
        }
        if (!ids.insert(game->entry.id).second) {
            return fail(ErrorCode::ValidationFailed, fmt::format("a second game with the id \"{}\"", game->entry.id),
                        at.child("id"));
        }
        catalog.games.push_back(std::move(*game));
    }
    if (const auto profiles = document.find("profiles"); profiles != document.end()) {
        // A profile document of its own, so that the issues point into this file ("<file>:/profiles/0/...").
        Json profileDocument = Json::object();
        profileDocument["schemaVersion"] = launcher::kEmulatorProfilesSchemaVersion;
        profileDocument["profiles"] = *profiles;
        catalog.profiles = launcher::parseProfileDocument(profileDocument, fileName, launcher::ProfileOrigin::User);
    }
    return catalog;
}

Expected<MockCatalog> loadMockCatalog(const std::filesystem::path& file) {
    auto document = core::readJsonFile(file);
    if (!document) {
        return propagate(document.error());
    }
    return parseMockCatalog(*document, file);
}

launcher::EmulatorProfiles mockCatalogProfiles(launcher::EmulatorProfiles base, const MockCatalog& catalog) {
    launcher::ProfileDocument effective{.profiles = std::move(base.profiles),
                                        .systemChoices = std::move(base.systemChoices),
                                        .issues = std::move(base.issues)};
    return launcher::mergeProfileDocuments(std::move(effective), catalog.profiles);
}

} // namespace rv::app
