#include <rv/catalog_db/catalog_store.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/snapshot.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

namespace rv::catalog_db {

using catalog::Availability;
using catalog::CatalogEntry;
using catalog::EntryState;
using catalog::FileFacts;
using catalog::GameId;
using catalog::IndexedEntry;
using catalog::LibraryId;
using catalog::LibraryProfile;
using catalog::LookupKey;
using catalog::LookupKeyVersion;
using catalog::MediaKind;
using catalog::RawElement;
using catalog::RawEsdeValues;
using catalog::StoredIdentity;
using catalog::SystemInfo;
using catalog::SystemName;

namespace {

/// How long a commit's checkpoint waits for readers of older snapshots before it is left to the next one.
constexpr auto kCheckpointWait = std::chrono::milliseconds(200);

constexpr std::int64_t kMaxStoredInteger = std::numeric_limits<std::int64_t>::max();

// --- Values ----------------------------------------------------------------------------------------------------------

std::string lossy(const std::filesystem::path& path) {
    return core::pathToUtf8Lossy(path);
}

Error withPrefix(Error error, std::string_view prefix) {
    error.message = fmt::format("{}: {}", prefix, error.message);
    return error;
}

/// True when `error` is SQLite's report of a failed constraint of `kind` ("FOREIGN KEY", "UNIQUE"): its code alone
/// stands for other failures too.
bool isConstraintFailure(const Error& error, std::string_view kind) {
    return error.message.find(fmt::format("{} constraint failed", kind)) != std::string::npos;
}

/// A value the database holds that the catalog cannot read back: a damaged or foreign row.
tl::unexpected<Error> unreadable(std::string_view what, std::string context) {
    return makeError(ErrorCode::ParseError, fmt::format("the catalog database holds {}", what), std::move(context));
}

/// `value` as SQLite's signed 64-bit integer. Errors: InvalidArgument beyond 2^63 - 1.
Expected<std::int64_t> storedInteger(std::uint64_t value, std::string_view what) {
    if (value > static_cast<std::uint64_t>(kMaxStoredInteger)) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("{} {} is beyond what the database holds (2^63 - 1)", what, value));
    }
    return static_cast<std::int64_t>(value);
}

/// An integer read back as unsigned: what the store wrote is never negative.
Expected<std::uint64_t> readUnsigned(std::int64_t value, std::string_view what, const std::string& context) {
    if (value < 0) {
        return unreadable(fmt::format("a negative {} ({})", what, value), context);
    }
    return static_cast<std::uint64_t>(value);
}

/// UTF-8 text of a path, exact. Errors: InvalidArgument for a path that is not valid Unicode.
Expected<std::string> pathText(const std::filesystem::path& path, std::string_view what) {
    auto text = core::tryPathToUtf8(path);
    if (!text) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("{} is not valid Unicode", what), lossy(path));
    }
    return text;
}

Expected<std::optional<std::string>> optionalPathText(const std::optional<std::filesystem::path>& path,
                                                      std::string_view what) {
    if (!path) {
        return std::optional<std::string>();
    }
    auto text = pathText(*path, what);
    if (!text) {
        return propagate(text.error());
    }
    return std::optional<std::string>(std::move(*text));
}

Expected<std::filesystem::path> readPath(std::string_view text, const std::string& context) {
    auto path = core::tryPathFromUtf8(text);
    if (!path) {
        return unreadable("a path that is not valid UTF-8", context);
    }
    return path;
}

/// A release date as YYYYMMDD, which sorts like the dates. Errors: InvalidArgument for a date that is none.
Expected<std::int64_t> storedDate(const catalog::ReleaseDate& date) {
    if (date.year < 1 || date.year > 9999 || date.month < 1 || date.month > 12 || date.day < 1 || date.day > 31) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("the release date {}-{}-{} is no date", date.year, date.month, date.day));
    }
    return std::int64_t{date.year} * 10000 + date.month * 100 + date.day;
}

catalog::ReleaseDate readDate(std::int64_t value) noexcept {
    return catalog::ReleaseDate{static_cast<int>(value / 10000), static_cast<int>(value / 100 % 100),
                                static_cast<int>(value % 100)};
}

/// `json` as compact text. Errors: InvalidArgument when it holds text that is not UTF-8.
Expected<std::string> jsonText(const nlohmann::json& json, std::string_view what) {
    try {
        return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
    } catch (const nlohmann::json::exception&) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("{} are not valid UTF-8", what));
    }
}

// The raw ES-DE values of every scraped entry go through JSON at each import, both ways: a writer and a reader of their
// one shape, {"unknown":[["name","text"],...],"original":[...]} (RFC 8259 text), cost a fraction of a general JSON
// library's, in Debug builds above all.

/// Appends `text` (valid UTF-8) as a JSON string: '"', '\' and the control characters escaped, the rest as it is.
void appendJsonString(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

/// The raw ES-DE values as JSON, a member per list that is not empty; nothing when there are none. Errors:
/// InvalidArgument for text that is not UTF-8.
Expected<std::optional<std::string>> rawJson(const RawEsdeValues& raw) {
    if (raw.empty()) {
        return std::optional<std::string>();
    }
    std::string json = "{";
    const auto appendList = [&](std::string_view key, const std::vector<RawElement>& elements) {
        if (elements.empty()) {
            return true;
        }
        if (json.size() > 1) {
            json += ',';
        }
        appendJsonString(json, key);
        json += ":[";
        for (std::size_t i = 0; i < elements.size(); ++i) {
            if (!core::isValidUtf8(elements[i].name) || !core::isValidUtf8(elements[i].text)) {
                return false;
            }
            json += i == 0 ? "[" : ",[";
            appendJsonString(json, elements[i].name);
            json += ',';
            appendJsonString(json, elements[i].text);
            json += ']';
        }
        json += ']';
        return true;
    };
    if (!appendList("unknown", raw.unknownElements) || !appendList("original", raw.originalForms)) {
        return makeError(ErrorCode::InvalidArgument, "the raw ES-DE values are not valid UTF-8");
    }
    json += '}';
    return std::optional<std::string>(std::move(json));
}

/// Reads what rawJson() writes (white space between the tokens allowed, any string escape of RFC 8259 read).
class RawJsonReader {
public:
    explicit RawJsonReader(std::string_view text) noexcept : m_text(text) {}

    /// False for anything but an object whose members "unknown" and "original" are lists of [string, string] pairs.
    [[nodiscard]] bool read(RawEsdeValues& raw) {
        if (!take('{')) {
            return false;
        }
        if (!take('}')) {
            do {
                std::string key;
                if (!readString(key) || !take(':')) {
                    return false;
                }
                std::vector<RawElement>* elements = key == "unknown"    ? &raw.unknownElements
                                                    : key == "original" ? &raw.originalForms
                                                                        : nullptr;
                if (elements == nullptr || !elements->empty() || !readPairs(*elements)) {
                    return false;
                }
            } while (take(','));
            if (!take('}')) {
                return false;
            }
        }
        skipSpace();
        return m_at == m_text.size();
    }

private:
    void skipSpace() noexcept {
        while (m_at < m_text.size() &&
               (m_text[m_at] == ' ' || m_text[m_at] == '\n' || m_text[m_at] == '\r' || m_text[m_at] == '\t')) {
            ++m_at;
        }
    }

    /// Skips white space, then takes `c` when it comes next.
    bool take(char c) noexcept {
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == c) {
            ++m_at;
            return true;
        }
        return false;
    }

    bool readPairs(std::vector<RawElement>& elements) {
        if (!take('[')) {
            return false;
        }
        if (take(']')) {
            return true;
        }
        do {
            RawElement element;
            if (!take('[') || !readString(element.name) || !take(',') || !readString(element.text) || !take(']')) {
                return false;
            }
            elements.push_back(std::move(element));
        } while (take(','));
        return take(']');
    }

    bool readHex4(char32_t& value) noexcept {
        if (m_text.size() - m_at < 4) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_at++];
            const int digit = c >= '0' && c <= '9'   ? c - '0'
                              : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                     : -1;
            if (digit < 0) {
                return false;
            }
            value = value * 16 + static_cast<char32_t>(digit);
        }
        return true;
    }

    /// A string, its escapes decoded; false for anything else, a lone surrogate and text that is not UTF-8.
    bool readString(std::string& out) {
        if (!take('"')) {
            return false;
        }
        while (m_at < m_text.size()) {
            const char c = m_text[m_at++];
            if (c == '"') {
                return core::isValidUtf8(out);
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_at == m_text.size()) {
                return false;
            }
            switch (const char escaped = m_text[m_at++]; escaped) {
            case '"':
            case '\\':
            case '/':
                out += escaped;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                char32_t codePoint = 0;
                if (!readHex4(codePoint) || (codePoint >= 0xDC00 && codePoint <= 0xDFFF)) {
                    return false;
                }
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    // A high surrogate: its low one follows, as \uXXXX.
                    char32_t low = 0;
                    if (m_text.size() - m_at < 2 || m_text[m_at] != '\\' || m_text[m_at + 1] != 'u') {
                        return false;
                    }
                    m_at += 2;
                    if (!readHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return false;
                    }
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                }
                core::appendUtf8(out, codePoint);
                break;
            }
            default:
                return false;
            }
        }
        return false; // no closing quote
    }

    std::string_view m_text;
    std::size_t m_at = 0;
};

Expected<RawEsdeValues> readRaw(std::string_view text, const std::string& context) {
    RawEsdeValues raw;
    if (!RawJsonReader(text).read(raw)) {
        return unreadable("raw ES-DE values that are not a JSON object of [name, text] lists", context);
    }
    return raw;
}

/// An enumeration stored as its toString() name, read back with its parse function.
template <class Enum, class Parse>
Expected<Enum> readEnum(std::string_view text, Parse parse, std::string_view what, const std::string& context) {
    const std::optional<Enum> value = parse(text);
    if (!value) {
        return unreadable(fmt::format("an unknown {} '{}'", what, text), context);
    }
    return *value;
}

/// Sorts `values` by `less`, a total order, unless they are in order already: through a permutation, so that each value
/// (an entry with its metadata: large) moves once.
template <class T, class Less>
void sortByPermutation(std::vector<T>& values, Less less) {
    if (std::is_sorted(values.begin(), values.end(), less)) {
        return;
    }
    std::vector<std::size_t> order(values.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return less(values[a], values[b]); });
    std::vector<T> sorted;
    sorted.reserve(values.size());
    for (const std::size_t index : order) {
        sorted.push_back(std::move(values[index]));
    }
    values = std::move(sorted);
}

/// The lookup key of a stored relative path, computed again (LookupKey is only ever made by makeLookupKey).
Expected<LookupKey> readKey(std::string_view relativePath, const std::string& context) {
    auto key = catalog::makeLookupKey(relativePath);
    if (!key) {
        return unreadable(fmt::format("a relative path that makes no lookup key ({})", key.error().message), context);
    }
    return key;
}

// --- Statements ------------------------------------------------------------------------------------------------------

/// The columns of idx_games, in the order of the store's SELECT and upsert (bind index = column + 1).
enum class GameColumn : int {
    Id,
    Library,
    System,
    RelativePath,
    State,
    NotAGameReason,
    ExcludingSetting,
    Availability,
    UnavailableReason,
    IsDirectory,
    FileSize,
    FileTime,
    FileHidden,
    GamelistPath,
    Title,
    SortKey,
    Name,
    SortName,
    Description,
    Developer,
    Publisher,
    Genre,
    ReleaseDate,
    Rating,
    PlayersMin,
    PlayersMax,
    DiscHeadPath,
    AltEmulator,
    Favorite,
    Hidden,
    KidGame,
    Completed,
    Broken,
    PlayCount,
    PlayTime,
    LastPlayed,
    MediaMask,
    Raw,
    Count,
};

constexpr std::array<std::string_view, static_cast<std::size_t>(GameColumn::Count)> kGameColumnNames{
    "id",
    "library",
    "system",
    "relative_path",
    "state",
    "not_a_game_reason",
    "excluding_setting",
    "availability",
    "unavailable_reason",
    "is_directory",
    "file_size",
    "file_time",
    "file_hidden",
    "gamelist_path",
    "title",
    "sort_key",
    "name",
    "sort_name",
    "description",
    "developer",
    "publisher",
    "genre",
    "release_date",
    "rating",
    "players_min",
    "players_max",
    "disc_head_path",
    "alt_emulator",
    "favorite",
    "hidden",
    "kid_game",
    "completed",
    "broken",
    "play_count",
    "play_time",
    "last_played",
    "media_mask",
    "raw",
};

constexpr int column(GameColumn value) noexcept {
    return static_cast<int>(value);
}
constexpr int parameter(GameColumn value) noexcept {
    return static_cast<int>(value) + 1;
}

/// The columns of an identity, as the store's identity SELECTs return them.
constexpr std::string_view kIdentityColumns = "id, library, system, relative_path, key_rule, key_casing, present, "
                                              "is_directory, file_size, file_time, file_hidden";

/// The statements of the store, prepared on first use and kept.
enum class Sql : std::size_t {
    SelectLibraries,
    UpsertLibrary,
    SelectIdentities,
    SelectIdentity,
    SelectTakenIds,
    UpsertIdentity,
    SelectKeyVersions,
    UpsertMeta,
    SelectIdentitySystem,
    UpsertSystem,
    CountGames,
    SelectGames,
    SelectMedia,
    UpsertGame,
    DeleteMedia,
    InsertMedia,
    DeleteDiscMembers,
    DeleteDiscGroup,
    DeleteGame,
    Count,
};

std::string gameUpsertSql() {
    std::string parameters;
    std::string updates;
    for (std::size_t i = 0; i < kGameColumnNames.size(); ++i) {
        parameters += fmt::format("{}?{}", i == 0 ? "" : ", ", i + 1);
        if (i > 0) {
            updates += fmt::format("{}{} = excluded.{}", updates.empty() ? "" : ", ", kGameColumnNames[i],
                                   kGameColumnNames[i]);
        }
    }
    // An upsert, never REPLACE: REPLACE deletes the row without firing delete triggers (the search index's, step 9).
    return fmt::format("INSERT INTO idx_games ({}) VALUES ({}) ON CONFLICT (id) DO UPDATE SET {}",
                       fmt::join(kGameColumnNames, ", "), parameters, updates);
}

std::string sqlOf(Sql which) {
    switch (which) {
    case Sql::SelectLibraries:
        return "SELECT id, adapter, esde_home, install_folder, rom_root, media_root FROM rv_libraries ORDER BY id";
    case Sql::UpsertLibrary:
        return "INSERT INTO rv_libraries (id, adapter, esde_home, install_folder, rom_root, media_root) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT (id) DO UPDATE SET adapter = excluded.adapter, "
               "esde_home = excluded.esde_home, install_folder = excluded.install_folder, "
               "rom_root = excluded.rom_root, media_root = excluded.media_root";
    case Sql::SelectIdentities:
        return fmt::format("SELECT {} FROM rv_game_identities WHERE library = ?1 AND system = ?2 "
                           "ORDER BY lookup_key, relative_path",
                           kIdentityColumns);
    case Sql::SelectIdentity:
        return fmt::format("SELECT {} FROM rv_game_identities WHERE id = ?1", kIdentityColumns);
    case Sql::SelectTakenIds:
        return "SELECT id FROM rv_game_identities UNION SELECT alias FROM rv_identity_aliases ORDER BY 1";
    case Sql::UpsertIdentity:
        // first_seen once, when the identity is made; last_seen whenever an import sees its file.
        return "INSERT INTO rv_game_identities (id, library, system, lookup_key, relative_path, key_rule, key_casing, "
               "present, is_directory, file_size, file_time, file_hidden, first_seen, last_seen) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, CASE WHEN ?8 THEN ?13 END) "
               "ON CONFLICT (id) DO UPDATE SET library = excluded.library, system = excluded.system, "
               "lookup_key = excluded.lookup_key, relative_path = excluded.relative_path, "
               "key_rule = excluded.key_rule, key_casing = excluded.key_casing, present = excluded.present, "
               "is_directory = excluded.is_directory, file_size = excluded.file_size, "
               "file_time = excluded.file_time, file_hidden = excluded.file_hidden, "
               "last_seen = coalesce(excluded.last_seen, last_seen)";
    case Sql::SelectKeyVersions:
        return "SELECT key_rule, key_casing FROM rv_game_identities GROUP BY key_rule, key_casing LIMIT 2";
    case Sql::UpsertMeta:
        return "INSERT INTO rv_meta (key, value) VALUES (?1, ?2) ON CONFLICT (key) DO UPDATE SET value = "
               "excluded.value";
    case Sql::SelectIdentitySystem:
        return "SELECT library, system FROM rv_game_identities WHERE id = ?1";
    case Sql::UpsertSystem:
        return "INSERT INTO idx_systems (library, name, full_name, platforms, theme, rom_folder_configured, "
               "rom_folder, availability, unavailable_reason, gamelist, alternative_emulator) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11) ON CONFLICT (library, name) DO UPDATE SET "
               "full_name = excluded.full_name, platforms = excluded.platforms, theme = excluded.theme, "
               "rom_folder_configured = excluded.rom_folder_configured, rom_folder = excluded.rom_folder, "
               "availability = excluded.availability, unavailable_reason = excluded.unavailable_reason, "
               "gamelist = excluded.gamelist, alternative_emulator = excluded.alternative_emulator";
    case Sql::CountGames:
        return "SELECT count(*) FROM idx_games WHERE library = ?1 AND system = ?2";
    case Sql::SelectGames:
        return fmt::format("SELECT {} FROM idx_games WHERE library = ?1 AND system = ?2 ORDER BY id",
                           fmt::join(kGameColumnNames, ", "));
    case Sql::SelectMedia:
        return "SELECT m.game, m.kind, m.extension FROM idx_media AS m JOIN idx_games AS g ON g.id = m.game "
               "WHERE g.library = ?1 AND g.system = ?2 ORDER BY m.game, m.kind";
    case Sql::UpsertGame:
        return gameUpsertSql();
    case Sql::DeleteMedia:
        return "DELETE FROM idx_media WHERE game = ?1";
    case Sql::InsertMedia:
        return "INSERT INTO idx_media (game, kind, extension) VALUES (?1, ?2, ?3)";
    case Sql::DeleteDiscMembers:
        return "DELETE FROM idx_disc_members WHERE head = ?1";
    case Sql::DeleteDiscGroup:
        return "DELETE FROM idx_disc_groups WHERE head = ?1";
    case Sql::DeleteGame:
        return "DELETE FROM idx_games WHERE id = ?1";
    case Sql::Count:
        break;
    }
    return {};
}

// --- Rows ------------------------------------------------------------------------------------------------------------

/// An identity from a row of the identity SELECTs (kIdentityColumns). `library` and `system` are the row's, parsed by
/// the caller when it does not know them.
Expected<StoredIdentity> readIdentity(const Statement& row, const LibraryId& library, const SystemName& system) {
    const std::int64_t value = row.columnInt(0);
    const std::string context = fmt::format("rv_game_identities row {}", value);
    const auto id = GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
    if (!id) {
        return unreadable(fmt::format("an identity whose GameId {} is none", value), context);
    }
    auto key = readKey(row.columnText(3), context);
    if (!key) {
        return propagate(key.error());
    }
    const std::int64_t rule = row.columnInt(4);
    if (rule < 0 || rule > std::numeric_limits<std::uint32_t>::max()) {
        return unreadable(fmt::format("a lookup key rule {} out of range", rule), context);
    }
    StoredIdentity identity;
    identity.id = *id;
    identity.library = library;
    identity.system = system;
    identity.key = std::move(*key);
    identity.keyVersion = LookupKeyVersion{static_cast<std::uint32_t>(rule), std::string(row.columnText(5))};
    identity.present = row.columnInt(6) != 0;
    if (!row.isNull(7)) {
        const auto size = readUnsigned(row.columnInt(8), "file size", context);
        if (!size) {
            return propagate(size.error());
        }
        identity.file = FileFacts{row.columnInt(7) != 0, *size, row.columnInt(9), row.columnInt(10) != 0};
    }
    return identity;
}

/// An identity from a row whose library and system are not known yet (identity(id)).
Expected<StoredIdentity> readIdentityAnywhere(const Statement& row) {
    const std::string context = fmt::format("rv_game_identities row {}", row.columnInt(0));
    const auto library = LibraryId::parse(row.columnText(1));
    if (!library) {
        return unreadable(fmt::format("an invalid library id ({})", library.error().message), context);
    }
    const auto system = SystemName::parse(row.columnText(2));
    if (!system) {
        return unreadable(fmt::format("an invalid system name ({})", system.error().message), context);
    }
    return readIdentity(row, *library, *system);
}

/// An index row from a row of the games SELECT (kGameColumnNames), its media left empty (idx_media gives them).
Expected<IndexedEntry> readGame(const Statement& row, const SystemName& system) {
    const auto text = [&](GameColumn which) {
        return row.columnText(column(which));
    };
    const auto integer = [&](GameColumn which) {
        return row.columnInt(column(which));
    };
    const auto isNull = [&](GameColumn which) {
        return row.isNull(column(which));
    };

    const std::int64_t value = integer(GameColumn::Id);
    const std::string context = fmt::format("idx_games row {}", value);
    const auto id = GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
    if (!id) {
        return unreadable(fmt::format("an index row whose GameId {} is none", value), context);
    }
    IndexedEntry indexed;
    indexed.id = *id;
    CatalogEntry& entry = indexed.entry;
    entry.system = system;
    auto key = readKey(text(GameColumn::RelativePath), context);
    if (!key) {
        return propagate(key.error());
    }
    entry.key = std::move(*key);

    const auto state = readEnum<EntryState>(text(GameColumn::State), catalog::parseEntryState, "entry state", context);
    const auto reason = readEnum<catalog::NotAGameReason>(text(GameColumn::NotAGameReason),
                                                          catalog::parseNotAGameReason, "reason", context);
    const auto availability =
        readEnum<Availability>(text(GameColumn::Availability), catalog::parseAvailability, "availability", context);
    const auto unavailable = readEnum<catalog::SourceUnavailableReason>(
        text(GameColumn::UnavailableReason), catalog::parseSourceUnavailableReason, "unavailability", context);
    if (!state || !reason || !availability || !unavailable) {
        return propagate(!state          ? state.error()
                         : !reason       ? reason.error()
                         : !availability ? availability.error()
                                         : unavailable.error());
    }
    entry.state = *state;
    entry.notAGameReason = *reason;
    entry.excludingSetting = std::string(text(GameColumn::ExcludingSetting));
    entry.availability = *availability;
    entry.unavailableReason = *unavailable;
    if (!isNull(GameColumn::IsDirectory)) {
        const auto size = readUnsigned(integer(GameColumn::FileSize), "file size", context);
        if (!size) {
            return propagate(size.error());
        }
        entry.file = FileFacts{integer(GameColumn::IsDirectory) != 0, *size, integer(GameColumn::FileTime),
                               integer(GameColumn::FileHidden) != 0};
    }
    entry.gamelistPath = std::string(text(GameColumn::GamelistPath));
    entry.title = std::string(text(GameColumn::Title));
    indexed.sortKey = std::string(text(GameColumn::SortKey));

    catalog::GameMetadata& metadata = entry.metadata;
    metadata.name = std::string(text(GameColumn::Name));
    metadata.sortName = std::string(text(GameColumn::SortName));
    metadata.description = std::string(text(GameColumn::Description));
    metadata.developer = std::string(text(GameColumn::Developer));
    metadata.publisher = std::string(text(GameColumn::Publisher));
    metadata.genre = std::string(text(GameColumn::Genre));
    if (!isNull(GameColumn::ReleaseDate)) {
        metadata.releaseDate = readDate(integer(GameColumn::ReleaseDate));
    }
    if (!isNull(GameColumn::Rating)) {
        metadata.rating = catalog::Rating{static_cast<int>(integer(GameColumn::Rating))};
    }
    if (!isNull(GameColumn::PlayersMin)) {
        catalog::Players players;
        players.min = static_cast<std::uint32_t>(integer(GameColumn::PlayersMin));
        players.max = isNull(GameColumn::PlayersMax)
                          ? std::nullopt
                          : std::optional<std::uint32_t>(static_cast<std::uint32_t>(integer(GameColumn::PlayersMax)));
        metadata.players = players;
    }
    if (!isNull(GameColumn::DiscHeadPath)) {
        auto head = readKey(text(GameColumn::DiscHeadPath), context);
        if (!head) {
            return propagate(head.error());
        }
        entry.discHead = std::move(*head);
    }
    entry.altEmulator = std::string(text(GameColumn::AltEmulator));

    catalog::EsdeFlags& flags = entry.flags;
    flags.favorite = integer(GameColumn::Favorite) != 0;
    flags.hidden = integer(GameColumn::Hidden) != 0;
    flags.kidGame = integer(GameColumn::KidGame) != 0;
    flags.completed = integer(GameColumn::Completed) != 0;
    flags.broken = integer(GameColumn::Broken) != 0;
    const auto playCount = readUnsigned(integer(GameColumn::PlayCount), "play count", context);
    const auto playTime = readUnsigned(integer(GameColumn::PlayTime), "play time", context);
    if (!playCount || !playTime) {
        return propagate(!playCount ? playCount.error() : playTime.error());
    }
    flags.playCount = *playCount;
    flags.playTimeSeconds = *playTime;
    if (!isNull(GameColumn::LastPlayed)) {
        const auto lastPlayed = catalog::parseEsdeDateTime(text(GameColumn::LastPlayed));
        if (!lastPlayed) {
            return unreadable(fmt::format("a last played time that is none ({})", lastPlayed.error().message), context);
        }
        flags.lastPlayed = *lastPlayed;
    }
    if (!isNull(GameColumn::Raw)) {
        auto raw = readRaw(text(GameColumn::Raw), context);
        if (!raw) {
            return propagate(raw.error());
        }
        entry.raw = std::move(*raw);
    }
    return indexed;
}

/// Binds every column of `indexed`'s idx_games row, its library `library`. Errors: values the schema does not hold.
Status bindGame(Statement& statement, const IndexedEntry& indexed, std::string_view library) {
    const CatalogEntry& entry = indexed.entry;
    const catalog::GameMetadata& metadata = entry.metadata;
    const auto id = storedInteger(indexed.id.value(), "the GameId");
    if (!id) {
        return propagate(id.error());
    }
    statement.bindInt(parameter(GameColumn::Id), *id)
        .bindText(parameter(GameColumn::Library), library)
        .bindText(parameter(GameColumn::System), entry.system.str())
        .bindText(parameter(GameColumn::RelativePath), entry.key.relativePath())
        .bindText(parameter(GameColumn::State), catalog::toString(entry.state))
        .bindText(parameter(GameColumn::NotAGameReason), catalog::toString(entry.notAGameReason))
        .bindText(parameter(GameColumn::ExcludingSetting), entry.excludingSetting)
        .bindText(parameter(GameColumn::Availability), catalog::toString(entry.availability))
        .bindText(parameter(GameColumn::UnavailableReason), catalog::toString(entry.unavailableReason));
    if (entry.file) {
        const auto size = storedInteger(entry.file->size, "the file size");
        if (!size) {
            return propagate(size.error());
        }
        statement.bindBool(parameter(GameColumn::IsDirectory), entry.file->directory)
            .bindInt(parameter(GameColumn::FileSize), *size)
            .bindInt(parameter(GameColumn::FileTime), entry.file->lastWriteTime)
            .bindBool(parameter(GameColumn::FileHidden), entry.file->hidden);
    } else {
        statement.bindNull(parameter(GameColumn::IsDirectory))
            .bindNull(parameter(GameColumn::FileSize))
            .bindNull(parameter(GameColumn::FileTime))
            .bindNull(parameter(GameColumn::FileHidden));
    }
    statement.bindText(parameter(GameColumn::GamelistPath), entry.gamelistPath)
        .bindText(parameter(GameColumn::Title), entry.title)
        .bindText(parameter(GameColumn::SortKey), indexed.sortKey)
        .bindText(parameter(GameColumn::Name), metadata.name)
        .bindText(parameter(GameColumn::SortName), metadata.sortName)
        .bindText(parameter(GameColumn::Description), metadata.description)
        .bindText(parameter(GameColumn::Developer), metadata.developer)
        .bindText(parameter(GameColumn::Publisher), metadata.publisher)
        .bindText(parameter(GameColumn::Genre), metadata.genre);
    if (metadata.releaseDate) {
        const auto date = storedDate(*metadata.releaseDate);
        if (!date) {
            return propagate(date.error());
        }
        statement.bindInt(parameter(GameColumn::ReleaseDate), *date);
    } else {
        statement.bindNull(parameter(GameColumn::ReleaseDate));
    }
    statement.bindOptionalInt(parameter(GameColumn::Rating),
                              metadata.rating ? std::optional<std::int64_t>(metadata.rating->tenths) : std::nullopt);
    if (metadata.players) {
        statement.bindInt(parameter(GameColumn::PlayersMin), metadata.players->min)
            .bindOptionalInt(parameter(GameColumn::PlayersMax),
                             metadata.players->max ? std::optional<std::int64_t>(*metadata.players->max)
                                                   : std::nullopt);
    } else {
        statement.bindNull(parameter(GameColumn::PlayersMin)).bindNull(parameter(GameColumn::PlayersMax));
    }
    statement
        .bindOptionalText(parameter(GameColumn::DiscHeadPath),
                          entry.discHead.empty() ? std::nullopt
                                                 : std::optional<std::string_view>(entry.discHead.relativePath()))
        .bindText(parameter(GameColumn::AltEmulator), entry.altEmulator);

    const catalog::EsdeFlags& flags = entry.flags;
    const auto playCount = storedInteger(flags.playCount, "the play count");
    const auto playTime = storedInteger(flags.playTimeSeconds, "the play time");
    if (!playCount || !playTime) {
        return propagate(!playCount ? playCount.error() : playTime.error());
    }
    statement.bindBool(parameter(GameColumn::Favorite), flags.favorite)
        .bindBool(parameter(GameColumn::Hidden), flags.hidden)
        .bindBool(parameter(GameColumn::KidGame), flags.kidGame)
        .bindBool(parameter(GameColumn::Completed), flags.completed)
        .bindBool(parameter(GameColumn::Broken), flags.broken)
        .bindInt(parameter(GameColumn::PlayCount), *playCount)
        .bindInt(parameter(GameColumn::PlayTime), *playTime);
    if (flags.lastPlayed) {
        // Stored as ES-DE writes it, and only when that text reads back as the same time (a read refuses any other).
        const std::string lastPlayed = catalog::formatEsdeDateTime(*flags.lastPlayed);
        const auto readBack = catalog::parseEsdeDateTime(lastPlayed);
        if (!readBack || *readBack != *flags.lastPlayed) {
            return makeError(ErrorCode::InvalidArgument,
                             fmt::format("the last played time {} is no date and time", lastPlayed));
        }
        statement.bindText(parameter(GameColumn::LastPlayed), lastPlayed);
    } else {
        statement.bindNull(parameter(GameColumn::LastPlayed));
    }
    statement.bindInt(parameter(GameColumn::MediaMask), indexed.media.presenceMask());
    const auto raw = rawJson(entry.raw);
    if (!raw) {
        return propagate(raw.error());
    }
    statement.bindOptionalText(parameter(GameColumn::Raw),
                               raw->has_value() ? std::optional<std::string_view>(**raw) : std::nullopt);
    return ok();
}

/// Steps a statement that returns at most one row, reads it with `read(statement, hasRow)`, then resets the statement.
template <class Read>
auto readSingleRow(Statement& statement, Read read) -> decltype(read(statement, true)) {
    const ScopedReset reset(statement);
    const auto row = statement.step();
    if (!row) {
        return propagate(row.error());
    }
    return read(statement, *row);
}

} // namespace

// --- State and the import transaction --------------------------------------------------------------------------------

struct SqliteCatalogStore::State {
    explicit State(Database& database) noexcept : database(database) {}

    /// The statement `which`, prepared on first use and kept (on the writer connection).
    Expected<Statement*> statement(Sql which) {
        Statement& kept = statements[static_cast<std::size_t>(which)];
        if (!kept.valid()) {
            auto prepared = database.writer().prepare(sqlOf(which));
            if (!prepared) {
                return propagate(prepared.error());
            }
            kept = std::move(*prepared);
        }
        return &kept;
    }

    Database& database;
    std::array<Statement, static_cast<std::size_t>(Sql::Count)> statements;
    /// The import transaction this store has open, if any.
    ImportTransaction* open = nullptr;
};

class SqliteCatalogStore::ImportTransaction final : public catalog::IImportTransaction {
public:
    ImportTransaction(State& state, Transaction transaction, LibraryId library, std::string now) noexcept
        : m_state(&state), m_transaction(std::move(transaction)), m_library(std::move(library)), m_now(std::move(now)) {
    }
    ImportTransaction(const ImportTransaction&) = delete;
    ImportTransaction& operator=(const ImportTransaction&) = delete;
    ~ImportTransaction() override { finish(); }

    Status putLibrary(const LibraryProfile& profile) override;
    Status putIdentity(const StoredIdentity& identity) override;
    Status putSystem(const LibraryId& library, const SystemInfo& system) override;
    Status putEntry(const IndexedEntry& entry) override;
    Status removeEntry(GameId id) override;
    Status commit() override;

    /// The store is going: the transaction rolls back, and every later call fails.
    void detach() noexcept { finish(); }

private:
    /// Ok while the transaction is open. SQLite rolls a whole transaction back on its own after some errors (a full
    /// disk, an I/O error, an interrupt): the transaction then ends, so that nothing is written outside it.
    Status active() {
        if (m_state == nullptr) {
            return makeError(ErrorCode::InvalidArgument, "the import transaction has ended");
        }
        if (!m_state->database.writer().inTransaction()) {
            finish();
            return makeError(ErrorCode::InvalidArgument,
                             "the import transaction has ended: SQLite rolled it back after an error");
        }
        return ok();
    }

    /// The error of a write, the transaction ended when SQLite rolled it back with it.
    Error failed(Error error) {
        if (m_state != nullptr && !m_state->database.writer().inTransaction()) {
            finish();
            error.message += " (SQLite rolled the import transaction back)";
        }
        return error;
    }

    /// Rolls back unless committed, releases the import lock and leaves the store.
    void finish() noexcept {
        if (m_state == nullptr) {
            return;
        }
        m_transaction.rollback(); // nothing once committed, or rolled back by SQLite
        m_state->database.lock().unlockImport();
        if (m_state->open == this) {
            m_state->open = nullptr;
        }
        m_state = nullptr;
    }

    Expected<Statement*> statement(Sql which) { return m_state->statement(which); }

    /// Records in rv_meta the lookup key version of every identity's key, or "mixed" while they differ.
    Status recordKeyVersion();

    State* m_state;
    Transaction m_transaction;
    LibraryId m_library;
    /// The import's time: identities first seen and seen by it carry it.
    std::string m_now;
};

Status SqliteCatalogStore::ImportTransaction::putLibrary(const LibraryProfile& profile) {
    if (auto open = active(); !open) {
        return open;
    }
    const auto home = pathText(profile.esdeHome, "the ES-DE home");
    const auto install = optionalPathText(profile.installFolder, "the ES-DE install folder");
    const auto romRoot = optionalPathText(profile.romRoot, "the ROM root");
    const auto mediaRoot = optionalPathText(profile.mediaRoot, "the media root");
    if (!home || !install || !romRoot || !mediaRoot) {
        return propagate(!home      ? home.error()
                         : !install ? install.error()
                         : !romRoot ? romRoot.error()
                                    : mediaRoot.error());
    }
    auto upsert = statement(Sql::UpsertLibrary);
    if (!upsert) {
        return propagate(upsert.error());
    }
    (*upsert)
        ->bindText(1, profile.id.str())
        .bindText(2, catalog::toString(profile.adapter))
        .bindText(3, *home)
        .bindOptionalText(4, *install)
        .bindOptionalText(5, *romRoot)
        .bindOptionalText(6, *mediaRoot);
    if (auto stored = (*upsert)->run(); !stored) {
        return propagate(
            failed(withPrefix(stored.error(), fmt::format("cannot store the library '{}'", profile.id.str()))));
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::recordKeyVersion() {
    // At most two of the identities' versions: one says what every key is, two that they differ.
    auto select = statement(Sql::SelectKeyVersions);
    if (!select) {
        return propagate(select.error());
    }
    std::vector<std::pair<std::string, std::string>> versions;
    {
        Statement& row = **select;
        const ScopedReset reset(row);
        while (true) {
            const auto next = row.step();
            if (!next) {
                return propagate(next.error());
            }
            if (!*next) {
                break;
            }
            versions.emplace_back(std::to_string(row.columnInt(0)), std::string(row.columnText(1)));
        }
    }
    if (versions.empty()) {
        return ok(); // no identity yet: the versions of the system that made the schema stay
    }
    if (versions.size() > 1) {
        versions = {{"mixed", "mixed"}}; // a re-keying not finished
    }
    auto upsert = statement(Sql::UpsertMeta);
    if (!upsert) {
        return propagate(upsert.error());
    }
    for (const auto& [key, value] :
         {std::pair<std::string_view, std::string_view>{"lookup_key_rule", versions[0].first},
          {"casing_table_version", versions[0].second}}) {
        (*upsert)->bindText(1, key).bindText(2, value);
        if (auto stored = (*upsert)->run(); !stored) {
            return stored;
        }
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::putIdentity(const StoredIdentity& identity) {
    if (auto open = active(); !open) {
        return open;
    }
    const std::string context = identity.id.toString();
    if (!identity.id.isValid() || identity.system.empty() || identity.key.empty()) {
        return makeError(ErrorCode::InvalidArgument, "an identity needs a GameId, a system and a key", context);
    }
    std::optional<std::int64_t> size;
    if (identity.file) {
        const auto stored = storedInteger(identity.file->size, "the file size");
        if (!stored) {
            return propagate(withPrefix(stored.error(), "cannot store the identity " + context));
        }
        size = *stored;
    }
    auto upsert = statement(Sql::UpsertIdentity);
    if (!upsert) {
        return propagate(upsert.error());
    }
    Statement& row = **upsert;
    row.bindInt(1, static_cast<std::int64_t>(identity.id.value()))
        .bindText(2, identity.library.str())
        .bindText(3, identity.system.str())
        .bindText(4, identity.key.key())
        .bindText(5, identity.key.relativePath())
        .bindInt(6, identity.keyVersion.rule)
        .bindText(7, identity.keyVersion.casingTable)
        .bindBool(8, identity.present)
        .bindText(13, m_now);
    if (identity.file) {
        row.bindBool(9, identity.file->directory)
            .bindInt(10, *size)
            .bindInt(11, identity.file->lastWriteTime)
            .bindBool(12, identity.file->hidden);
    } else {
        row.bindNull(9).bindNull(10).bindNull(11).bindNull(12);
    }
    if (auto stored = row.run(); !stored) {
        Error error = stored.error();
        if (isConstraintFailure(error, "FOREIGN KEY")) {
            error.message =
                fmt::format("its library '{}' is not in the store ({})", identity.library.str(), error.message);
        } else if (isConstraintFailure(error, "UNIQUE")) {
            error.message = fmt::format("another identity has its key and relative path '{}' ({})",
                                        identity.key.relativePath(), error.message);
        }
        return propagate(failed(withPrefix(std::move(error), "cannot store the identity " + context)));
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::putSystem(const LibraryId& library, const SystemInfo& system) {
    if (auto open = active(); !open) {
        return open;
    }
    const std::string context = library.str() + "/" + system.name.str();
    if (system.name.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a system needs a name", context);
    }
    const auto platforms = jsonText(nlohmann::json(system.platforms), "the platforms");
    const auto romFolder = pathText(system.romFolder, "the ROM folder");
    if (!platforms || !romFolder) {
        return propagate(!platforms ? platforms.error() : romFolder.error());
    }
    auto upsert = statement(Sql::UpsertSystem);
    if (!upsert) {
        return propagate(upsert.error());
    }
    (*upsert)
        ->bindText(1, library.str())
        .bindText(2, system.name.str())
        .bindText(3, system.fullName)
        .bindText(4, *platforms)
        .bindText(5, system.theme)
        .bindText(6, system.romFolderConfigured)
        .bindText(7, *romFolder)
        .bindText(8, catalog::toString(system.availability))
        .bindText(9, catalog::toString(system.unavailableReason))
        .bindText(10, catalog::toString(system.gamelist))
        .bindText(11, system.alternativeEmulator);
    if (auto stored = (*upsert)->run(); !stored) {
        Error error = stored.error();
        if (isConstraintFailure(error, "FOREIGN KEY")) {
            error.message = fmt::format("its library '{}' is not in the store ({})", library.str(), error.message);
        }
        return propagate(failed(withPrefix(std::move(error), "cannot store the system " + context)));
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::putEntry(const IndexedEntry& indexed) {
    if (auto open = active(); !open) {
        return open;
    }
    const CatalogEntry& entry = indexed.entry;
    // Made for errors only: an import indexes every entry of the library.
    const auto context = [&] {
        return indexed.id.toString() + " " + entry.system.str() + "/" + entry.key.relativePath();
    };
    if (!indexed.id.isValid() || entry.system.empty()) {
        return makeError(ErrorCode::InvalidArgument, "an index row needs a GameId and a system", context());
    }
    if (entry.key.empty()) {
        return makeError(ErrorCode::InvalidArgument,
                         "an entry without a key (outside its system folder) is never indexed", context());
    }
    for (const MediaKind kind : catalog::kMediaKinds) {
        const catalog::MediaPresence& media = indexed.media[kind];
        if (!media.present && !media.extension.empty()) {
            return makeError(ErrorCode::InvalidArgument,
                             fmt::format("an absent media kind ({}) has an extension", catalog::toString(kind)),
                             context());
        }
    }

    // Its identity: it must exist, in the same system; its library is the row's.
    auto owner = statement(Sql::SelectIdentitySystem);
    if (!owner) {
        return propagate(owner.error());
    }
    (*owner)->bindInt(1, static_cast<std::int64_t>(indexed.id.value()));
    const auto library =
        readSingleRow(**owner, [&](const Statement& row, bool hasRow) -> Expected<std::optional<std::string>> {
            if (!hasRow) {
                return std::optional<std::string>();
            }
            if (row.columnText(1) != entry.system.str()) {
                return makeError(ErrorCode::InvalidArgument,
                                 fmt::format("the entry's system is not its identity's ({})", row.columnText(1)),
                                 context());
            }
            return std::optional<std::string>(std::string(row.columnText(0)));
        });
    if (!library) {
        return propagate(failed(library.error()));
    }
    if (!library->has_value()) {
        return makeError(ErrorCode::InvalidArgument, "no identity has this GameId: store the identity first",
                         context());
    }

    auto upsert = statement(Sql::UpsertGame);
    if (!upsert) {
        return propagate(upsert.error());
    }
    if (auto bound = bindGame(**upsert, indexed, **library); !bound) {
        (*upsert)->reset(); // the bindings made before the value it refused
        return propagate(withPrefix(bound.error(), "cannot index the entry " + context()));
    }
    if (auto stored = (*upsert)->run(); !stored) {
        return propagate(failed(withPrefix(stored.error(), "cannot index the entry " + context())));
    }

    // Its media: the kinds present, each with its extension.
    auto clear = statement(Sql::DeleteMedia);
    auto insert = statement(Sql::InsertMedia);
    if (!clear || !insert) {
        return propagate(!clear ? clear.error() : insert.error());
    }
    const auto id = static_cast<std::int64_t>(indexed.id.value());
    (*clear)->bindInt(1, id);
    if (auto cleared = (*clear)->run(); !cleared) {
        return propagate(failed(withPrefix(cleared.error(), "cannot index the media of " + context())));
    }
    for (const MediaKind kind : catalog::kMediaKinds) {
        const catalog::MediaPresence& media = indexed.media[kind];
        if (!media.present) {
            continue;
        }
        (*insert)->bindInt(1, id).bindInt(2, static_cast<std::int64_t>(kind)).bindText(3, media.extension);
        if (auto stored = (*insert)->run(); !stored) {
            return propagate(failed(withPrefix(stored.error(), "cannot index the media of " + context())));
        }
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::removeEntry(GameId id) {
    if (auto open = active(); !open) {
        return open;
    }
    if (!id.isValid()) {
        return makeError(ErrorCode::InvalidArgument, "no GameId to remove");
    }
    // The rows that refer to the game first, then its own; its identity stays. The disc group it heads goes with it; a
    // group it is a member of is its head's, which keeps it as it was read (CachedDiscGroup).
    for (const Sql which : {Sql::DeleteMedia, Sql::DeleteDiscMembers, Sql::DeleteDiscGroup, Sql::DeleteGame}) {
        auto remove = statement(which);
        if (!remove) {
            return propagate(remove.error());
        }
        (*remove)->bindInt(1, static_cast<std::int64_t>(id.value()));
        if (auto removed = (*remove)->run(); !removed) {
            return propagate(failed(withPrefix(removed.error(), "cannot remove the index rows of " + id.toString())));
        }
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::commit() {
    if (auto open = active(); !open) {
        return open;
    }
    // rv_meta says which lookup key version the keys are of, as this import leaves them.
    if (auto recorded = recordKeyVersion(); !recorded) {
        finish();
        return propagate(withPrefix(recorded.error(), "cannot commit the import (it was rolled back)"));
    }
    Database& database = m_state->database;
    auto committed = m_transaction.commit();
    finish();
    if (!committed) {
        return propagate(withPrefix(committed.error(), "cannot commit the import (it was rolled back)"));
    }
    spdlog::debug("Catalog database: import of library '{}' committed", m_library.str());
    // A checkpoint after each import (ADR-0008 §5): the WAL copied into the database file and truncated, unless
    // readers of older snapshots keep it from completing for long; the next checkpoint does it then.
    if (auto checkpointed = database.checkpoint(kCheckpointWait); !checkpointed) {
        spdlog::debug("Catalog database: the checkpoint after the import is left to later: {}",
                      checkpointed.error().toString());
    }
    return ok();
}

// --- SqliteCatalogStore ----------------------------------------------------------------------------------------------

SqliteCatalogStore::SqliteCatalogStore(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}

SqliteCatalogStore::~SqliteCatalogStore() {
    if (m_state != nullptr && m_state->open != nullptr) {
        spdlog::warn("Catalog database: the store went away with its import transaction open: rolled back");
        m_state->open->detach();
    }
}

Expected<std::unique_ptr<SqliteCatalogStore>> SqliteCatalogStore::open(Database& database) {
    const auto version = database.schemaVersion();
    if (!version) {
        return propagate(version.error());
    }
    const int program = programSchemaVersion();
    if (*version < program) {
        return makeError(ErrorCode::Conflict,
                         fmt::format("the catalog database has schema version {}, this program needs {}: migrate it "
                                     "first (retroverse-cli db migrate)",
                                     *version, program),
                         lossy(database.path()));
    }
    if (*version > program) {
        return makeError(ErrorCode::Conflict,
                         fmt::format("the catalog database has schema version {}, newer than this program's {}: it "
                                     "was written by a newer Retroverse",
                                     *version, program),
                         lossy(database.path()));
    }
    return std::unique_ptr<SqliteCatalogStore>(new SqliteCatalogStore(std::make_unique<State>(database)));
}

Expected<std::vector<LibraryProfile>> SqliteCatalogStore::libraries() {
    auto select = m_state->statement(Sql::SelectLibraries);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    std::vector<LibraryProfile> libraries;
    while (true) {
        const auto next = row.step();
        if (!next) {
            return propagate(next.error());
        }
        if (!*next) {
            return libraries;
        }
        const std::string context = fmt::format("rv_libraries row '{}'", row.columnText(0));
        const auto id = LibraryId::parse(row.columnText(0));
        if (!id) {
            return unreadable(fmt::format("an invalid library id ({})", id.error().message), context);
        }
        const auto adapter =
            readEnum<catalog::LibraryAdapter>(row.columnText(1), catalog::parseLibraryAdapter, "adapter", context);
        if (!adapter) {
            return propagate(adapter.error());
        }
        LibraryProfile profile;
        profile.id = *id;
        profile.adapter = *adapter;
        auto home = readPath(row.columnText(2), context);
        if (!home) {
            return propagate(home.error());
        }
        profile.esdeHome = std::move(*home);
        const std::array<std::pair<int, std::optional<std::filesystem::path>*>, 3> optionalPaths{{
            {3, &profile.installFolder},
            {4, &profile.romRoot},
            {5, &profile.mediaRoot},
        }};
        for (const auto& [index, target] : optionalPaths) {
            if (row.isNull(index)) {
                continue;
            }
            auto path = readPath(row.columnText(index), context);
            if (!path) {
                return propagate(path.error());
            }
            *target = std::move(*path);
        }
        libraries.push_back(std::move(profile));
    }
}

Expected<std::vector<StoredIdentity>> SqliteCatalogStore::identities(const LibraryId& library,
                                                                     const SystemName& system) {
    auto select = m_state->statement(Sql::SelectIdentities);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindText(1, library.str()).bindText(2, system.str());
    std::vector<StoredIdentity> identities;
    while (true) {
        const auto next = row.step();
        if (!next) {
            return propagate(next.error());
        }
        if (!*next) {
            break;
        }
        auto identity = readIdentity(row, library, system);
        if (!identity) {
            return propagate(identity.error());
        }
        identities.push_back(std::move(*identity));
    }
    // The port's order is the keys': the stored ones, which SQL ordered, may be of an older casing table than the keys
    // computed again.
    sortByPermutation(identities, [](const StoredIdentity& a, const StoredIdentity& b) { return a.key < b.key; });
    return identities;
}

Expected<std::optional<StoredIdentity>> SqliteCatalogStore::identity(GameId id) {
    auto select = m_state->statement(Sql::SelectIdentity);
    if (!select) {
        return propagate(select.error());
    }
    (*select)->bindInt(1, static_cast<std::int64_t>(id.value()));
    return readSingleRow(**select, [](const Statement& row, bool hasRow) -> Expected<std::optional<StoredIdentity>> {
        if (!hasRow) {
            return std::optional<StoredIdentity>();
        }
        auto identity = readIdentityAnywhere(row);
        if (!identity) {
            return propagate(identity.error());
        }
        return std::optional<StoredIdentity>(std::move(*identity));
    });
}

Expected<std::vector<GameId>> SqliteCatalogStore::takenGameIds() {
    auto select = m_state->statement(Sql::SelectTakenIds);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    std::vector<GameId> taken;
    while (true) {
        const auto next = row.step();
        if (!next) {
            return propagate(next.error());
        }
        if (!*next) {
            return taken;
        }
        const std::int64_t value = row.columnInt(0);
        const auto id = GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
        if (!id) {
            return unreadable(fmt::format("a GameId {} that is none", value),
                              "rv_game_identities, rv_identity_aliases");
        }
        taken.push_back(*id);
    }
}

Expected<std::vector<IndexedEntry>> SqliteCatalogStore::indexedEntries(const LibraryId& library,
                                                                       const SystemName& system) {
    Connection& writer = m_state->database.writer();
    // Two reads (the rows, their media) of one snapshot: a read transaction of their own, unless the import's is open.
    std::optional<Transaction> snapshot;
    if (!writer.inTransaction()) {
        auto begun = Transaction::begin(writer, Transaction::Mode::Deferred);
        if (!begun) {
            return propagate(begun.error());
        }
        snapshot = std::move(*begun);
    }

    std::vector<IndexedEntry> entries;
    {
        // Room for every row at once: entries are large to move.
        auto count = m_state->statement(Sql::CountGames);
        if (!count) {
            return propagate(count.error());
        }
        (*count)->bindText(1, library.str()).bindText(2, system.str());
        const auto rows = readSingleRow(**count, [](const Statement& row, bool hasRow) -> Expected<std::int64_t> {
            return hasRow ? row.columnInt(0) : 0;
        });
        if (!rows) {
            return propagate(rows.error());
        }
        entries.reserve(static_cast<std::size_t>(std::max<std::int64_t>(*rows, 0)));
    }
    {
        auto select = m_state->statement(Sql::SelectGames);
        if (!select) {
            return propagate(select.error());
        }
        Statement& row = **select;
        const ScopedReset reset(row);
        row.bindText(1, library.str()).bindText(2, system.str());
        while (true) {
            const auto next = row.step();
            if (!next) {
                return propagate(next.error());
            }
            if (!*next) {
                break;
            }
            auto entry = readGame(row, system);
            if (!entry) {
                return propagate(entry.error());
            }
            entries.push_back(std::move(*entry));
        }
    }
    {
        // Both reads are in GameId order: the media join their rows in one pass.
        auto select = m_state->statement(Sql::SelectMedia);
        if (!select) {
            return propagate(select.error());
        }
        Statement& row = **select;
        const ScopedReset reset(row);
        row.bindText(1, library.str()).bindText(2, system.str());
        auto entry = entries.begin();
        while (true) {
            const auto next = row.step();
            if (!next) {
                return propagate(next.error());
            }
            if (!*next) {
                break;
            }
            const std::int64_t game = row.columnInt(0);
            const std::int64_t kind = row.columnInt(1);
            while (entry != entries.end() && static_cast<std::int64_t>(entry->id.value()) < game) {
                ++entry;
            }
            if (entry == entries.end() || static_cast<std::int64_t>(entry->id.value()) != game) {
                return unreadable("media of an entry that is not indexed", fmt::format("idx_media row {}", game));
            }
            if (kind < 0 || kind >= static_cast<std::int64_t>(catalog::kMediaKindCount)) {
                return unreadable(fmt::format("an unknown media kind {}", kind), fmt::format("idx_media row {}", game));
            }
            entry->media.kinds[static_cast<std::size_t>(kind)] =
                catalog::MediaPresence{true, std::string(row.columnText(2))};
        }
    }
    sortByPermutation(
        entries, [](const IndexedEntry& a, const IndexedEntry& b) { return catalog::entryOrder(a.entry, b.entry); });
    return entries;
}

Expected<std::unique_ptr<catalog::IImportTransaction>> SqliteCatalogStore::beginImport(const LibraryId& library) {
    if (m_state->open != nullptr) {
        return makeError(ErrorCode::Conflict, "this store's import transaction is still open", library.str());
    }
    // The import lock first, without waiting: an import of another process gets Conflict at once.
    DatabaseLock& lock = m_state->database.lock();
    if (auto locked = lock.lockImport(); !locked) {
        return propagate(locked.error());
    }
    auto transaction = Transaction::begin(m_state->database.writer(), Transaction::Mode::Immediate);
    if (!transaction) {
        lock.unlockImport();
        return propagate(withPrefix(transaction.error(), "cannot begin the import"));
    }
    auto open = std::make_unique<ImportTransaction>(*m_state, std::move(*transaction), library, core::utcNow());
    m_state->open = open.get();
    spdlog::debug("Catalog database: import of library '{}' begun", library.str());
    return std::unique_ptr<catalog::IImportTransaction>(std::move(open));
}

} // namespace rv::catalog_db
