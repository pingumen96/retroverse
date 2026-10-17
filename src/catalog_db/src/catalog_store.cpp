#include <rv/catalog_db/catalog_store.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/catalog/snapshot.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/catalog_db/search_index.hpp>
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
                                              "is_directory, file_size, file_time, file_hidden, quick_hash, "
                                              "quick_hash_size, quick_hash_time";

/// The columns of a history row, as the history SELECT returns them.
constexpr std::string_view kHistoryColumns =
    "id, game, at, event, reason, from_key, from_path, to_key, to_path, alias, review, detail";

/// The columns of a review row with its identity's system, as the review SELECTs return them.
constexpr std::string_view kReviewColumns = "r.id, r.game, i.system, r.vanished_path, r.candidates, r.reason, "
                                            "r.created_at, r.resolved_at, r.resolution, r.resolved_with";

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
    Savepoint,
    ReleaseSavepoint,
    RollBackToSavepoint,
    // I2 step 11a: the import's reads and writes.
    SelectSystems,
    SelectIdentitySystems,
    SelectDiscGroups,
    SelectDiscMembers,
    SelectDiscReferences,
    SelectLastImport,
    CountIndexRows,
    UpsertDiscGroup,
    InsertDiscMember,
    DeleteDiscReferences,
    InsertDiscReference,
    DeleteSystem,
    DeleteImportIssues,
    DeleteSystemImportIssues,
    InsertImportIssue,
    InsertImportRecord,
    ClearMedia,
    ClearDiscMembers,
    ClearDiscReferences,
    ClearDiscGroups,
    ClearGames,
    ClearSystems,
    PruneImportRecords,
    DeleteDiscMembership,
    InsertAlias,
    DeleteIdentity,
    // I2 step 11b: reconciliation's reads and writes.
    UpdateQuickHash,
    InsertHistory,
    SelectHistory,
    InsertReview,
    ResolveReview,
    ResolveReviewsOf,
    SelectReviewState,
    SelectReviews,
    SelectReview,
    SelectAliasesOf,
    SelectAlias,
    SelectStateReferences,
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
        // first_seen once, when the identity is made; last_seen whenever it is written with its file present (an
        // import writes an identity only when it changes: first seen, its path, file facts or quick hash changed,
        // re-keyed). The quick hash is the identity's (StoredIdentity::quickHash, read with it).
        return "INSERT INTO rv_game_identities (id, library, system, lookup_key, relative_path, key_rule, key_casing, "
               "present, is_directory, file_size, file_time, file_hidden, first_seen, last_seen, quick_hash, "
               "quick_hash_size, quick_hash_time) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, CASE WHEN ?8 THEN ?13 END, ?14, ?15, ?16) "
               "ON CONFLICT (id) DO UPDATE SET library = excluded.library, system = excluded.system, "
               "lookup_key = excluded.lookup_key, relative_path = excluded.relative_path, "
               "key_rule = excluded.key_rule, key_casing = excluded.key_casing, present = excluded.present, "
               "is_directory = excluded.is_directory, file_size = excluded.file_size, "
               "file_time = excluded.file_time, file_hidden = excluded.file_hidden, "
               "last_seen = coalesce(excluded.last_seen, last_seen), quick_hash = excluded.quick_hash, "
               "quick_hash_size = excluded.quick_hash_size, quick_hash_time = excluded.quick_hash_time";
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
    case Sql::Savepoint:
        return "SAVEPOINT rv_store_write";
    case Sql::ReleaseSavepoint:
        return "RELEASE rv_store_write";
    case Sql::RollBackToSavepoint:
        return "ROLLBACK TO rv_store_write";
    case Sql::SelectSystems:
        return "SELECT name, full_name, platforms, theme, rom_folder_configured, rom_folder, availability, "
               "unavailable_reason, gamelist, alternative_emulator FROM idx_systems WHERE library = ?1 ORDER BY name";
    case Sql::SelectIdentitySystems:
        // Read from the unique index on (library, system, relative path), without visiting the rows.
        return "SELECT DISTINCT system FROM rv_game_identities WHERE library = ?1 ORDER BY system";
    case Sql::SelectDiscGroups:
        return "SELECT g.head, g.kind, g.head_size, g.head_time, i.system, i.relative_path FROM idx_disc_groups AS g "
               "JOIN rv_game_identities AS i ON i.id = g.head WHERE i.library = ?1 ORDER BY g.head";
    case Sql::SelectDiscMembers:
        return "SELECT m.head, m.member, i.relative_path FROM idx_disc_members AS m "
               "JOIN rv_game_identities AS h ON h.id = m.head JOIN rv_game_identities AS i ON i.id = m.member "
               "WHERE h.library = ?1 ORDER BY m.head, m.position";
    case Sql::SelectDiscReferences:
        return "SELECT r.head, r.relative_path FROM idx_disc_references AS r "
               "JOIN rv_game_identities AS h ON h.id = r.head WHERE h.library = ?1 ORDER BY r.head, r.position";
    case Sql::SelectLastImport:
        return "SELECT id, kind, started_at, finished_at, accept_removals, systems_asked, summary FROM rv_imports "
               "WHERE library = ?1 ORDER BY id DESC LIMIT 1";
    case Sql::CountIndexRows:
        return "SELECT count(*) FROM idx_games";
    case Sql::UpsertDiscGroup:
        return "INSERT INTO idx_disc_groups (head, kind, head_size, head_time) VALUES (?1, ?2, ?3, ?4) "
               "ON CONFLICT (head) DO UPDATE SET kind = excluded.kind, head_size = excluded.head_size, "
               "head_time = excluded.head_time";
    case Sql::InsertDiscMember:
        return "INSERT INTO idx_disc_members (head, position, member) VALUES (?1, ?2, ?3)";
    case Sql::DeleteDiscReferences:
        return "DELETE FROM idx_disc_references WHERE head = ?1";
    case Sql::InsertDiscReference:
        return "INSERT INTO idx_disc_references (head, position, relative_path) VALUES (?1, ?2, ?3)";
    case Sql::DeleteSystem:
        return "DELETE FROM idx_systems WHERE library = ?1 AND name = ?2";
    case Sql::DeleteImportIssues:
        return "DELETE FROM idx_import_issues WHERE library = ?1";
    case Sql::DeleteSystemImportIssues:
        return "DELETE FROM idx_import_issues WHERE library = ?1 AND system = ?2";
    case Sql::InsertImportIssue:
        return "INSERT INTO idx_import_issues (library, severity, code, system, file, reason) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6)";
    case Sql::InsertImportRecord:
        return "INSERT INTO rv_imports (library, kind, started_at, finished_at, accept_removals, systems_asked, "
               "summary) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)";
    case Sql::ClearMedia:
        return "DELETE FROM idx_media WHERE game IN (SELECT id FROM rv_game_identities WHERE library = ?1)";
    case Sql::ClearDiscMembers:
        return "DELETE FROM idx_disc_members WHERE head IN (SELECT id FROM rv_game_identities WHERE library = ?1)";
    case Sql::ClearDiscReferences:
        return "DELETE FROM idx_disc_references WHERE head IN (SELECT id FROM rv_game_identities WHERE library = ?1)";
    case Sql::ClearDiscGroups:
        return "DELETE FROM idx_disc_groups WHERE head IN (SELECT id FROM rv_game_identities WHERE library = ?1)";
    case Sql::ClearGames:
        return "DELETE FROM idx_games WHERE library = ?1";
    case Sql::ClearSystems:
        return "DELETE FROM idx_systems WHERE library = ?1";
    case Sql::PruneImportRecords:
        // The newest ?2 records of the library stay.
        return "DELETE FROM rv_imports WHERE library = ?1 AND id <= (SELECT id FROM rv_imports WHERE library = ?1 "
               "ORDER BY id DESC LIMIT 1 OFFSET ?2)";
    case Sql::DeleteDiscMembership:
        return "DELETE FROM idx_disc_members WHERE member = ?1";
    case Sql::InsertAlias:
        return "INSERT INTO rv_identity_aliases (alias, game, lookup_key, relative_path, at, reason) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6)";
    case Sql::DeleteIdentity:
        return "DELETE FROM rv_game_identities WHERE id = ?1";
    case Sql::UpdateQuickHash:
        return "UPDATE rv_game_identities SET quick_hash = ?2, quick_hash_size = ?3, quick_hash_time = ?4 WHERE id = ?1";
    case Sql::InsertHistory:
        return "INSERT INTO rv_identity_history (game, at, event, reason, from_key, from_path, to_key, to_path, alias, "
               "review, detail) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)";
    case Sql::SelectHistory:
        return fmt::format("SELECT {} FROM rv_identity_history WHERE game = ?1 ORDER BY id", kHistoryColumns);
    case Sql::InsertReview:
        return "INSERT INTO rv_identity_reviews (game, vanished_key, vanished_path, candidates, reason, created_at) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6)";
    case Sql::ResolveReview:
        return "UPDATE rv_identity_reviews SET resolved_at = ?2, resolution = ?3, resolved_with = ?4 "
               "WHERE id = ?1 AND resolved_at IS NULL";
    case Sql::ResolveReviewsOf:
        return "UPDATE rv_identity_reviews SET resolved_at = ?2, resolution = ?3, resolved_with = ?4 "
               "WHERE game = ?1 AND resolved_at IS NULL";
    case Sql::SelectReviewState:
        return "SELECT resolved_at IS NULL FROM rv_identity_reviews WHERE id = ?1";
    case Sql::SelectReviews:
        return fmt::format("SELECT {} FROM rv_identity_reviews AS r JOIN rv_game_identities AS i ON i.id = r.game "
                           "WHERE i.library = ?1 AND (?2 = 0 OR r.resolved_at IS NULL) ORDER BY r.id",
                           kReviewColumns);
    case Sql::SelectReview:
        return fmt::format("SELECT {} FROM rv_identity_reviews AS r JOIN rv_game_identities AS i ON i.id = r.game "
                           "WHERE r.id = ?1",
                           kReviewColumns);
    case Sql::SelectAliasesOf:
        return "SELECT alias, game, relative_path, at, reason FROM rv_identity_aliases WHERE game = ?1 ORDER BY alias";
    case Sql::SelectAlias:
        return "SELECT alias, game, relative_path, at, reason FROM rv_identity_aliases WHERE alias = ?1";
    case Sql::SelectStateReferences:
        // Every column of a state table (rv_*) that names an identity through a foreign key of its own, whatever step
        // made the table: what putAlias() moves to the identity an alias stands for. Index tables (idx_*) are not
        // state: putAlias() removes their rows. Only single-column keys into the identity's GameId (or its primary key).
        return "SELECT m.name, f.\"from\" FROM sqlite_schema AS m JOIN pragma_foreign_key_list(m.name) AS f "
               "WHERE m.type = 'table' AND substr(m.name, 1, 3) = 'rv_' AND f.\"table\" = 'rv_game_identities' "
               "COLLATE NOCASE AND (f.\"to\" IS NULL OR f.\"to\" = 'id' COLLATE NOCASE) AND f.seq = 0 "
               "AND NOT EXISTS (SELECT 1 FROM pragma_foreign_key_list(m.name) AS g WHERE g.id = f.id AND g.seq > 0) "
               "ORDER BY m.name, f.id";
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
    if (!row.isNull(11)) {
        const std::span<const std::byte> digest = row.columnBlob(11);
        const auto size = readUnsigned(row.columnInt(12), "quick hash size", context);
        if (!size) {
            return propagate(size.error());
        }
        if (digest.size() != std::tuple_size_v<core::Sha256Digest>) {
            return unreadable(fmt::format("a quick hash of {} bytes", digest.size()), context);
        }
        catalog::QuickHash hash;
        std::transform(digest.begin(), digest.end(), hash.digest.begin(),
                       [](std::byte b) { return static_cast<std::uint8_t>(b); });
        hash.size = *size;
        hash.lastWriteTime = row.columnInt(13);
        identity.quickHash = hash;
    }
    return identity;
}

/// Binds a quick hash to three parameters from `first` (digest, size, time), or NULL to all three. Errors:
/// InvalidArgument for a size beyond 2^63 - 1.
Status bindQuickHash(Statement& statement, int first, const std::optional<catalog::QuickHash>& hash) {
    if (!hash) {
        statement.bindNull(first).bindNull(first + 1).bindNull(first + 2);
        return ok();
    }
    const auto size = storedInteger(hash->size, "the quick hash's size");
    if (!size) {
        return propagate(size.error());
    }
    statement.bindBlob(first, std::as_bytes(std::span<const std::uint8_t>(hash->digest)))
        .bindInt(first + 1, *size)
        .bindInt(first + 2, hash->lastWriteTime);
    return ok();
}

/// A history row from a row of the history SELECT (kHistoryColumns).
Expected<catalog::IdentityEvent> readHistory(const Statement& row) {
    const std::string context = fmt::format("rv_identity_history row {}", row.columnInt(0));
    catalog::IdentityEvent event;
    event.id = row.columnInt(0);
    const std::int64_t game = row.columnInt(1);
    const auto id = GameId::fromValue(game < 0 ? 0 : static_cast<std::uint64_t>(game));
    if (!id) {
        return unreadable("a history row of no GameId", context);
    }
    event.game = *id;
    event.at = std::string(row.columnText(2));
    const auto kind = readEnum<catalog::IdentityEventKind>(row.columnText(3), catalog::parseIdentityEventKind,
                                                           "history event", context);
    if (!kind) {
        return propagate(kind.error());
    }
    event.kind = *kind;
    if (!row.columnText(4).empty()) {
        const auto reason = readEnum<catalog::MoveReason>(row.columnText(4), catalog::parseMoveReason, "move reason",
                                                         context);
        if (!reason) {
            return propagate(reason.error());
        }
        event.reason = *reason;
    }
    event.fromKey = std::string(row.columnText(5));
    event.fromPath = std::string(row.columnText(6));
    event.toKey = std::string(row.columnText(7));
    event.toPath = std::string(row.columnText(8));
    if (const auto alias = row.columnOptionalInt(9)) {
        event.alias = GameId::fromValue(*alias < 0 ? 0 : static_cast<std::uint64_t>(*alias));
        if (!event.alias) {
            return unreadable("a history row whose alias is no GameId", context);
        }
    }
    event.review = row.columnOptionalInt(10);
    event.detail = std::string(row.columnText(11));
    return event;
}

/// A review row from a row of the review SELECTs (kReviewColumns).
Expected<catalog::StoredReview> readReview(const Statement& row) {
    const std::string context = fmt::format("rv_identity_reviews row {}", row.columnInt(0));
    catalog::StoredReview stored;
    catalog::ReviewRow& review = stored.review;
    review.id = row.columnInt(0);
    const std::int64_t game = row.columnInt(1);
    const auto id = GameId::fromValue(game < 0 ? 0 : static_cast<std::uint64_t>(game));
    auto system = SystemName::parse(row.columnText(2));
    auto vanished = readKey(row.columnText(3), context);
    if (!id || !system || !vanished) {
        return !id ? unreadable("a review row of no GameId", context)
               : !system ? unreadable("a review row of an invalid system", context)
                         : propagate(vanished.error());
    }
    review.game = *id;
    review.system = std::move(*system);
    review.vanished = std::move(*vanished);
    try {
        for (const nlohmann::json& candidate : nlohmann::json::parse(row.columnText(4))) {
            auto key = readKey(candidate.get<std::string>(), context);
            if (!key) {
                return propagate(key.error());
            }
            review.candidates.push_back(std::move(*key));
        }
    } catch (const nlohmann::json::exception&) {
        return unreadable("a review row whose candidates are not a JSON array of paths", context);
    }
    review.reason = std::string(row.columnText(5));
    stored.createdAt = std::string(row.columnText(6));
    if (!row.isNull(7)) {
        stored.resolvedAt = std::string(row.columnText(7));
        const auto resolution = readEnum<catalog::ReviewResolution>(row.columnText(8), catalog::parseReviewResolution,
                                                                    "review resolution", context);
        if (!resolution) {
            return propagate(resolution.error());
        }
        stored.resolution = *resolution;
    }
    if (const auto with = row.columnOptionalInt(9)) {
        stored.resolvedWith = GameId::fromValue(*with < 0 ? 0 : static_cast<std::uint64_t>(*with));
        if (!stored.resolvedWith) {
            return unreadable("a review row resolved with no GameId", context);
        }
    }
    return stored;
}

/// An alias from a row of the alias SELECTs (alias, game, relative_path, at, reason).
Expected<catalog::StoredAlias> readAlias(const Statement& row) {
    const std::string context = fmt::format("rv_identity_aliases row {}", row.columnInt(0));
    const std::int64_t aliasValue = row.columnInt(0);
    const std::int64_t gameValue = row.columnInt(1);
    const auto alias = GameId::fromValue(aliasValue < 0 ? 0 : static_cast<std::uint64_t>(aliasValue));
    const auto game = GameId::fromValue(gameValue < 0 ? 0 : static_cast<std::uint64_t>(gameValue));
    auto key = readKey(row.columnText(2), context);
    const auto reason = readEnum<catalog::MoveReason>(row.columnText(4), catalog::parseMoveReason, "alias reason",
                                                     context);
    if (!alias || !game || !key || !reason) {
        return !alias || !game ? unreadable("an alias of no GameId", context)
               : !key          ? propagate(key.error())
                               : propagate(reason.error());
    }
    return catalog::StoredAlias{*alias, *game, std::move(*key), std::string(row.columnText(3)), *reason};
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
    const auto raw = rawJson(entry.raw);
    if (!raw) {
        return propagate(raw.error());
    }
    statement.bindOptionalText(parameter(GameColumn::Raw),
                               raw->has_value() ? std::optional<std::string_view>(**raw) : std::nullopt);
    return ok();
}

/// A system from a row of the systems SELECT (Sql::SelectSystems).
Expected<SystemInfo> readSystem(const Statement& row) {
    const std::string context = fmt::format("idx_systems row '{}'", row.columnText(0));
    auto name = SystemName::parse(row.columnText(0));
    if (!name) {
        return unreadable(fmt::format("an invalid system name ({})", name.error().message), context);
    }
    SystemInfo system;
    system.name = std::move(*name);
    system.fullName = std::string(row.columnText(1));
    try {
        const nlohmann::json platforms = nlohmann::json::parse(row.columnText(2));
        system.platforms = platforms.get<std::vector<std::string>>();
    } catch (const nlohmann::json::exception&) {
        return unreadable("platforms that are not a JSON array of strings", context);
    }
    system.theme = std::string(row.columnText(3));
    system.romFolderConfigured = std::string(row.columnText(4));
    auto folder = readPath(row.columnText(5), context);
    if (!folder) {
        return propagate(folder.error());
    }
    system.romFolder = std::move(*folder);
    const auto availability =
        readEnum<Availability>(row.columnText(6), catalog::parseAvailability, "availability", context);
    const auto reason = readEnum<catalog::SourceUnavailableReason>(
        row.columnText(7), catalog::parseSourceUnavailableReason, "unavailability", context);
    const auto gamelist =
        readEnum<catalog::GamelistState>(row.columnText(8), catalog::parseGamelistState, "gamelist state", context);
    if (!availability || !reason || !gamelist) {
        return propagate(!availability ? availability.error() : !reason ? reason.error() : gamelist.error());
    }
    system.availability = *availability;
    system.unavailableReason = *reason;
    system.gamelist = *gamelist;
    system.alternativeEmulator = std::string(row.columnText(9));
    return system;
}

/// The figures of one system of an import record, as rv_imports' summary holds them.
nlohmann::json systemChangesJson(const catalog::SystemChanges& changes) {
    return nlohmann::json{{"system", changes.system.str()},
                          {"outcome", std::string(catalog::toString(changes.outcome))},
                          {"added", changes.added},
                          {"changed", changes.changed},
                          {"removed", changes.removed},
                          {"unchanged", changes.unchanged},
                          {"unavailable", changes.unavailable},
                          {"kept", changes.kept},
                          {"newIdentities", changes.newIdentities},
                          {"vanished", changes.vanished},
                          {"returned", changes.returned},
                          {"rekeyed", changes.rekeyed},
                          {"moved", changes.moved},
                          {"presentBefore", changes.presentBefore},
                          {"gone", changes.gone},
                          {"issues", changes.issues}};
}

/// What systemChangesJson() writes, read back. Errors: ParseError.
Expected<catalog::SystemChanges> readSystemChanges(const nlohmann::json& json, const std::string& context) {
    try {
        catalog::SystemChanges changes;
        auto system = SystemName::parse(json.at("system").get<std::string>());
        const auto outcome = catalog::parseSystemOutcome(json.at("outcome").get<std::string>());
        if (!system || !outcome) {
            return unreadable("an import record whose system or outcome is unknown", context);
        }
        changes.system = std::move(*system);
        changes.outcome = *outcome;
        const std::array<std::pair<const char*, std::size_t*>, 14> counts{{
            {"added", &changes.added},
            {"changed", &changes.changed},
            {"removed", &changes.removed},
            {"unchanged", &changes.unchanged},
            {"unavailable", &changes.unavailable},
            {"kept", &changes.kept},
            {"newIdentities", &changes.newIdentities},
            {"vanished", &changes.vanished},
            {"returned", &changes.returned},
            {"rekeyed", &changes.rekeyed},
            {"moved", &changes.moved},
            {"presentBefore", &changes.presentBefore},
            {"gone", &changes.gone},
            {"issues", &changes.issues},
        }};
        for (const auto& [name, value] : counts) {
            *value = json.at(name).get<std::size_t>();
        }
        return changes;
    } catch (const nlohmann::json::exception&) {
        return unreadable("an import record whose figures cannot be read", context);
    }
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
    State(Database& database, const StoreOptions& options) noexcept : database(database), options(options) {}

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

    /// The statements that move the state rows naming an identity to another (putAlias()): per column of a state table
    /// with a foreign key into rv_game_identities, an UPDATE OR IGNORE and a DELETE of what it left. Found in the schema
    /// on first use (the schema does not change while the database is open: a migration needs it closed elsewhere).
    struct Repoint {
        std::string table;
        std::string column;
        Statement update;
        Statement remove;
    };
    Expected<std::vector<Repoint>*> repoints() {
        if (repointsFound) {
            return &repointStatements;
        }
        auto select = statement(Sql::SelectStateReferences);
        if (!select) {
            return propagate(select.error());
        }
        std::vector<std::pair<std::string, std::string>> columns;
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
                columns.emplace_back(std::string(row.columnText(0)), std::string(row.columnText(1)));
            }
        }
        // Identifiers quoted, a '"' doubled: names read from the schema itself.
        const auto quoted = [](std::string_view name) {
            std::string text = "\"";
            for (const char c : name) {
                text += c == '"' ? std::string("\"\"") : std::string(1, c);
            }
            return text + "\"";
        };
        std::vector<Repoint> found;
        for (const auto& [table, column] : columns) {
            auto update = database.writer().prepare(fmt::format("UPDATE OR IGNORE {0} SET {1} = ?2 WHERE {1} = ?1",
                                                                quoted(table), quoted(column)));
            auto remove =
                database.writer().prepare(fmt::format("DELETE FROM {0} WHERE {1} = ?1", quoted(table), quoted(column)));
            if (!update || !remove) {
                return propagate(!update ? update.error() : remove.error());
            }
            found.push_back(Repoint{table, column, std::move(*update), std::move(*remove)});
        }
        repointStatements = std::move(found);
        repointsFound = true;
        return &repointStatements;
    }

    Database& database;
    StoreOptions options;
    std::array<Statement, static_cast<std::size_t>(Sql::Count)> statements;
    std::vector<Repoint> repointStatements;
    bool repointsFound = false;
    /// The import transaction this store has open, if any.
    ImportTransaction* open = nullptr;
    /// The import lock lockImport() holds, if any.
    ImportLock* lock = nullptr;
};

/// The import lock held apart from a transaction (lockImport()).
class SqliteCatalogStore::ImportLock final : public catalog::IImportLock {
public:
    explicit ImportLock(State& state) noexcept : m_state(&state) { m_state->lock = this; }
    ImportLock(const ImportLock&) = delete;
    ImportLock& operator=(const ImportLock&) = delete;
    ~ImportLock() override;

    /// The store is going: it releases the lock itself.
    void detach() noexcept { m_state = nullptr; }

private:
    State* m_state;
};

class SqliteCatalogStore::ImportTransaction final : public catalog::IImportTransaction {
public:
    /// `ownsLock`: the transaction took the import lock and releases it when it ends (false: lockImport() holds it).
    ImportTransaction(State& state, Transaction transaction, LibraryId library, std::string now, bool ownsLock) noexcept
        : m_state(&state), m_transaction(std::move(transaction)), m_library(std::move(library)), m_now(std::move(now)),
          m_ownsLock(ownsLock) {}
    ImportTransaction(const ImportTransaction&) = delete;
    ImportTransaction& operator=(const ImportTransaction&) = delete;
    ~ImportTransaction() override { finish(); }

    Status putLibrary(const LibraryProfile& profile) override;
    Status putIdentity(const StoredIdentity& identity) override;
    Status putSystem(const LibraryId& library, const SystemInfo& system) override;
    Status putEntry(const IndexedEntry& entry) override;
    Status removeEntry(GameId id) override;
    Expected<bool> prepareWrites(std::size_t rows) override;
    Status clearIndex(const LibraryId& library) override;
    Status removeSystem(const LibraryId& library, const SystemName& system) override;
    Status putDiscGroup(const catalog::IndexedDiscGroup& group) override;
    Status removeDiscGroup(GameId head) override;
    Status putAlias(const catalog::IdentityAlias& alias) override;
    Status putQuickHash(GameId id, const catalog::QuickHash& hash) override;
    Status putHistory(const catalog::IdentityEvent& event) override;
    Expected<std::int64_t> putReview(const catalog::ReviewRow& review) override;
    Status resolveReview(std::int64_t id, catalog::ReviewResolution resolution, std::optional<GameId> with) override;
    Status resolveReviewsOf(GameId game, catalog::ReviewResolution resolution) override;
    Status putImportIssues(const LibraryId& library, std::span<const catalog::Issue> issues,
                           std::span<const SystemName> systems) override;
    Status recordImport(const catalog::ImportRecord& record) override;
    Status commit() override;

    /// The store is going: the transaction rolls back, and every later call fails.
    void detach() noexcept { finish(); }
    /// The import lock lockImport() held is going while the transaction is open: the transaction releases it.
    void adoptLock() noexcept { m_ownsLock = true; }

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

    /// Rolls back unless committed, releases the import lock (unless lockImport() holds it) and leaves the store. A
    /// rollback also undoes a suspension of the search index (DDL is transactional).
    void finish() noexcept {
        if (m_state == nullptr) {
            return;
        }
        m_transaction.rollback(); // nothing once committed, or rolled back by SQLite
        if (m_ownsLock) {
            m_state->database.lock().unlockImport();
        }
        if (m_state->open == this) {
            m_state->open = nullptr;
        }
        m_state = nullptr;
    }

    /// Runs the statement `which` with `library` bound to ?1 (the deletions of clearIndex()).
    Status runForLibrary(Sql which, const LibraryId& library);

    Expected<Statement*> statement(Sql which) { return m_state->statement(which); }

    /// Runs `write`, a write of several statements, as one: inside a savepoint, so that when SQLite refuses one of its
    /// statements after others ran (a full disk or an I/O error may undo only that statement), what the others wrote is
    /// undone too, and the transaction goes on as it was before the write.
    template <class Write>
    Status asOne(Write write);

    /// Records in rv_meta the lookup key version of every identity's key, or "mixed" while they differ.
    Status recordKeyVersion();

    State* m_state;
    Transaction m_transaction;
    LibraryId m_library;
    /// The import's time: identities first seen and seen by it carry it.
    std::string m_now;
    bool m_ownsLock = true;
    /// prepareWrites() suspended the search index: commit() rebuilds it.
    bool m_searchSuspended = false;
};

SqliteCatalogStore::ImportLock::~ImportLock() {
    if (m_state == nullptr) {
        return;
    }
    m_state->lock = nullptr;
    if (m_state->open != nullptr) {
        m_state->open->adoptLock(); // a transaction still open keeps the lock to its end
    } else {
        m_state->database.lock().unlockImport();
    }
}

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

template <class Write>
Status SqliteCatalogStore::ImportTransaction::asOne(Write write) {
    auto begin = statement(Sql::Savepoint);
    auto release = statement(Sql::ReleaseSavepoint);
    auto rollBack = statement(Sql::RollBackToSavepoint);
    if (!begin || !release || !rollBack) {
        return propagate(!begin ? begin.error() : !release ? release.error() : rollBack.error());
    }
    if (auto begun = (*begin)->run(); !begun) {
        return propagate(failed(begun.error()));
    }
    Status written = write();
    if (written) {
        if (auto released = (*release)->run(); !released) {
            return propagate(failed(released.error()));
        }
        return written;
    }
    // Undone, unless SQLite rolled the whole transaction back with the error (failed() has then ended this one).
    if (m_state != nullptr && m_state->database.writer().inTransaction()) {
        auto undone = (*rollBack)->run();
        if (undone) {
            undone = (*release)->run();
        }
        if (!undone) {
            // What the write left cannot be undone alone: the import goes rather than commit half an entry.
            finish();
            Error error = written.error();
            error.message += fmt::format(" (the import transaction was rolled back: the write could not be undone: {})",
                                         undone.error().message);
            return propagate(std::move(error));
        }
    }
    return written;
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
    if (auto bound = bindQuickHash(row, 14, identity.quickHash); !bound) {
        row.reset();
        return propagate(withPrefix(bound.error(), "cannot store the identity " + context));
    }
    if (auto stored = row.run(); !stored) {
        Error error = stored.error();
        if (isConstraintFailure(error, "FOREIGN KEY")) {
            error.message =
                fmt::format("its library '{}' is not in the store ({})", identity.library.str(), error.message);
        } else if (isConstraintFailure(error, "UNIQUE")) {
            error.message = fmt::format("another identity of its system has its relative path '{}' ({})",
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
    auto clear = statement(Sql::DeleteMedia);
    auto insert = statement(Sql::InsertMedia);
    if (!upsert || !clear || !insert) {
        return propagate(!upsert ? upsert.error() : !clear ? clear.error() : insert.error());
    }
    if (auto bound = bindGame(**upsert, indexed, **library); !bound) {
        (*upsert)->reset(); // the bindings made before the value it refused
        return propagate(withPrefix(bound.error(), "cannot index the entry " + context()));
    }
    // The row and its media (the kinds present, each with its extension) as one write.
    return asOne([&]() -> Status {
        if (auto stored = (*upsert)->run(); !stored) {
            return propagate(failed(withPrefix(stored.error(), "cannot index the entry " + context())));
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
    });
}

Status SqliteCatalogStore::ImportTransaction::removeEntry(GameId id) {
    if (auto open = active(); !open) {
        return open;
    }
    if (!id.isValid()) {
        return makeError(ErrorCode::InvalidArgument, "no GameId to remove");
    }
    // The rows that refer to the game first, then its own, as one write; its identity stays. The disc group it heads
    // goes with it (members and references); a group it is a member of is its head's, which keeps it as it was read.
    constexpr std::array<Sql, 5> kRemovals{Sql::DeleteMedia, Sql::DeleteDiscMembers, Sql::DeleteDiscReferences,
                                           Sql::DeleteDiscGroup, Sql::DeleteGame};
    std::array<Statement*, kRemovals.size()> removals{};
    for (std::size_t i = 0; i < kRemovals.size(); ++i) {
        auto remove = statement(kRemovals[i]);
        if (!remove) {
            return propagate(remove.error());
        }
        removals[i] = *remove;
    }
    return asOne([&]() -> Status {
        for (Statement* remove : removals) {
            remove->bindInt(1, static_cast<std::int64_t>(id.value()));
            if (auto removed = remove->run(); !removed) {
                return propagate(
                    failed(withPrefix(removed.error(), "cannot remove the index rows of " + id.toString())));
            }
        }
        return ok();
    });
}

Expected<bool> SqliteCatalogStore::ImportTransaction::prepareWrites(std::size_t rows) {
    if (auto open = active(); !open) {
        return propagate(open.error());
    }
    if (m_searchSuspended) {
        return true;
    }
    const SearchIndexPolicy policy = m_state->options.searchIndex;
    if (rows == 0 || policy == SearchIndexPolicy::FollowWrites) {
        return false;
    }
    bool suspend = policy == SearchIndexPolicy::Suspend;
    if (policy == SearchIndexPolicy::Automatic) {
        // A first import (no entry indexed, of any library), or rows that are a large share of the index: the rebuild
        // tokenizes every entry once, where the triggers flush a segment per row written (search_index.hpp).
        auto count = statement(Sql::CountIndexRows);
        if (!count) {
            return propagate(count.error());
        }
        const auto indexed = readSingleRow(**count, [](const Statement& row, bool hasRow) -> Expected<std::int64_t> {
            return hasRow ? row.columnInt(0) : 0;
        });
        if (!indexed) {
            return propagate(failed(indexed.error()));
        }
        const auto entries = static_cast<std::size_t>(std::max<std::int64_t>(*indexed, 0));
        suspend = entries == 0 || rows * kBulkWriteShare >= entries;
    }
    if (!suspend) {
        return false;
    }
    if (auto suspended = suspendSearchIndex(m_state->database.writer()); !suspended) {
        return propagate(failed(withPrefix(suspended.error(), "cannot suspend the search index for the import")));
    }
    m_searchSuspended = true;
    spdlog::debug("Catalog database: the search index is suspended for the import's {} row(s)", rows);
    return true;
}

Status SqliteCatalogStore::ImportTransaction::runForLibrary(Sql which, const LibraryId& library) {
    auto run = statement(which);
    if (!run) {
        return propagate(run.error());
    }
    (*run)->bindText(1, library.str());
    if (auto ran = (*run)->run(); !ran) {
        return propagate(failed(ran.error()));
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::clearIndex(const LibraryId& library) {
    if (auto open = active(); !open) {
        return open;
    }
    // The rows that refer to games first, then the games, the systems and the issues; the state rows stay.
    constexpr std::array<Sql, 7> kClears{Sql::ClearMedia,     Sql::ClearDiscMembers, Sql::ClearDiscReferences,
                                         Sql::ClearDiscGroups, Sql::ClearGames,      Sql::ClearSystems,
                                         Sql::DeleteImportIssues};
    return asOne([&]() -> Status {
        for (const Sql which : kClears) {
            if (auto cleared = runForLibrary(which, library); !cleared) {
                return propagate(withPrefix(cleared.error(), "cannot clear the index of library " + library.str()));
            }
        }
        return ok();
    });
}

Status SqliteCatalogStore::ImportTransaction::removeSystem(const LibraryId& library, const SystemName& system) {
    if (auto open = active(); !open) {
        return open;
    }
    auto remove = statement(Sql::DeleteSystem);
    if (!remove) {
        return propagate(remove.error());
    }
    (*remove)->bindText(1, library.str()).bindText(2, system.str());
    if (auto removed = (*remove)->run(); !removed) {
        return propagate(
            failed(withPrefix(removed.error(), "cannot remove the system " + library.str() + "/" + system.str())));
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::putDiscGroup(const catalog::IndexedDiscGroup& indexed) {
    if (auto open = active(); !open) {
        return open;
    }
    const catalog::DiscGroup& group = indexed.group;
    const std::string context = indexed.head.toString() + " " + group.system.str() + "/" + group.head.relativePath();
    if (!indexed.head.isValid() || indexed.members.size() != group.members.size() ||
        std::any_of(indexed.members.begin(), indexed.members.end(), [](GameId id) { return !id.isValid(); })) {
        return makeError(ErrorCode::InvalidArgument, "a disc group needs a head and a GameId for each member", context);
    }
    if (std::any_of(group.references.begin(), group.references.end(),
                    [](const LookupKey& reference) { return reference.empty(); })) {
        return makeError(ErrorCode::InvalidArgument, "a disc group's reference has no path", context);
    }
    const auto size = storedInteger(group.headSize, "the head's size");
    if (!size) {
        return propagate(withPrefix(size.error(), "cannot store the disc group " + context));
    }
    std::array<Statement*, 5> statements{};
    constexpr std::array<Sql, 5> kStatements{Sql::UpsertDiscGroup, Sql::DeleteDiscMembers, Sql::InsertDiscMember,
                                             Sql::DeleteDiscReferences, Sql::InsertDiscReference};
    for (std::size_t i = 0; i < kStatements.size(); ++i) {
        auto prepared = statement(kStatements[i]);
        if (!prepared) {
            return propagate(prepared.error());
        }
        statements[i] = *prepared;
    }
    Statement* const upsert = statements[0];
    Statement* const clearMembers = statements[1];
    Statement* const insertMember = statements[2];
    Statement* const clearReferences = statements[3];
    Statement* const insertReference = statements[4];
    const auto head = static_cast<std::int64_t>(indexed.head.value());
    const auto written = [&](Status status) -> Status {
        if (status) {
            return status;
        }
        Error error = status.error();
        if (isConstraintFailure(error, "FOREIGN KEY")) {
            error.message = fmt::format("the identity of its head or of a member is not in the store ({})",
                                        error.message);
        }
        return propagate(failed(withPrefix(std::move(error), "cannot store the disc group " + context)));
    };
    return asOne([&]() -> Status {
        upsert->bindInt(1, head)
            .bindText(2, catalog::toString(group.kind))
            .bindInt(3, *size)
            .bindInt(4, group.headLastWriteTime);
        if (auto stored = written(upsert->run()); !stored) {
            return stored;
        }
        clearMembers->bindInt(1, head);
        if (auto cleared = written(clearMembers->run()); !cleared) {
            return cleared;
        }
        for (std::size_t i = 0; i < indexed.members.size(); ++i) {
            insertMember->bindInt(1, head)
                .bindInt(2, static_cast<std::int64_t>(i))
                .bindInt(3, static_cast<std::int64_t>(indexed.members[i].value()));
            if (auto stored = written(insertMember->run()); !stored) {
                return stored;
            }
        }
        clearReferences->bindInt(1, head);
        if (auto cleared = written(clearReferences->run()); !cleared) {
            return cleared;
        }
        for (std::size_t i = 0; i < group.references.size(); ++i) {
            insertReference->bindInt(1, head)
                .bindInt(2, static_cast<std::int64_t>(i))
                .bindText(3, group.references[i].relativePath());
            if (auto stored = written(insertReference->run()); !stored) {
                return stored;
            }
        }
        return ok();
    });
}

Status SqliteCatalogStore::ImportTransaction::removeDiscGroup(GameId head) {
    if (auto open = active(); !open) {
        return open;
    }
    constexpr std::array<Sql, 3> kRemovals{Sql::DeleteDiscMembers, Sql::DeleteDiscReferences, Sql::DeleteDiscGroup};
    std::array<Statement*, kRemovals.size()> removals{};
    for (std::size_t i = 0; i < kRemovals.size(); ++i) {
        auto remove = statement(kRemovals[i]);
        if (!remove) {
            return propagate(remove.error());
        }
        removals[i] = *remove;
    }
    return asOne([&]() -> Status {
        for (Statement* remove : removals) {
            remove->bindInt(1, static_cast<std::int64_t>(head.value()));
            if (auto removed = remove->run(); !removed) {
                return propagate(
                    failed(withPrefix(removed.error(), "cannot remove the disc group of " + head.toString())));
            }
        }
        return ok();
    });
}

Status SqliteCatalogStore::ImportTransaction::putAlias(const catalog::IdentityAlias& alias) {
    if (auto open = active(); !open) {
        return open;
    }
    const StoredIdentity& identity = alias.alias;
    const std::string context = identity.id.toString() + " -> " + alias.game.toString();
    if (!identity.id.isValid() || !alias.game.isValid() || identity.id == alias.game) {
        return makeError(ErrorCode::InvalidArgument, "an alias needs two different GameIds", context);
    }
    // Both identities in the store, of one library and system.
    auto owner = statement(Sql::SelectIdentitySystem);
    if (!owner) {
        return propagate(owner.error());
    }
    using Owner = std::optional<std::pair<std::string, std::string>>;
    const auto ownerOf = [&](GameId id) -> Expected<Owner> {
        (*owner)->bindInt(1, static_cast<std::int64_t>(id.value()));
        return readSingleRow(**owner, [](const Statement& row, bool hasRow) -> Expected<Owner> {
            if (!hasRow) {
                return Owner();
            }
            return Owner(std::pair(std::string(row.columnText(0)), std::string(row.columnText(1))));
        });
    };
    const auto aliasOwner = ownerOf(identity.id);
    if (!aliasOwner) {
        return propagate(aliasOwner.error());
    }
    const auto gameOwner = ownerOf(alias.game);
    if (!gameOwner) {
        return propagate(gameOwner.error());
    }
    if (!*aliasOwner || !*gameOwner || **aliasOwner != **gameOwner) {
        return makeError(ErrorCode::InvalidArgument,
                         "an alias and the identity it stands for must both be in the store, of one library and system",
                         context);
    }
    constexpr std::array<Sql, 8> kStatements{Sql::DeleteMedia,          Sql::DeleteDiscMembers, Sql::DeleteDiscReferences,
                                             Sql::DeleteDiscGroup,      Sql::DeleteGame,        Sql::DeleteDiscMembership,
                                             Sql::InsertAlias,          Sql::DeleteIdentity};
    std::array<Statement*, kStatements.size()> statements{};
    for (std::size_t i = 0; i < kStatements.size(); ++i) {
        auto prepared = statement(kStatements[i]);
        if (!prepared) {
            return propagate(prepared.error());
        }
        statements[i] = *prepared;
    }
    auto closeReviews = statement(Sql::ResolveReviewsOf);
    auto repoints = m_state->repoints();
    if (!closeReviews || !repoints) {
        return propagate(!closeReviews ? closeReviews.error() : repoints.error());
    }
    const auto id = static_cast<std::int64_t>(identity.id.value());
    const auto game = static_cast<std::int64_t>(alias.game.value());
    return asOne([&]() -> Status {
        // Its open review rows are resolved: it is no identity any more (the rows move to `game` below).
        (*closeReviews)
            ->bindInt(1, id)
            .bindText(2, m_now)
            .bindText(3, catalog::toString(catalog::ReviewResolution::Aliased))
            .bindInt(4, game);
        if (auto closed = (*closeReviews)->run(); !closed) {
            return propagate(failed(withPrefix(closed.error(), "cannot resolve the review rows of " + context)));
        }
        // Every state row that names it moves to `game`, whatever table holds it (found in the schema: the history, the
        // review rows, the aliases that stood for it, and the tables of later steps, play sessions and launch choices
        // among them). UPDATE OR IGNORE then DELETE: where a table holds one row per game and `game` has its own, the
        // row of `game` stays and the alias's goes.
        for (State::Repoint& repoint : **repoints) {
            repoint.update.bindInt(1, id).bindInt(2, game);
            if (auto moved = repoint.update.run(); !moved) {
                return propagate(failed(withPrefix(
                    moved.error(), fmt::format("cannot move the rows of {}.{} that name {}", repoint.table,
                                               repoint.column, context))));
            }
            repoint.remove.bindInt(1, id);
            if (auto removed = repoint.remove.run(); !removed) {
                return propagate(failed(withPrefix(
                    removed.error(), fmt::format("cannot remove the rows of {}.{} that name {}", repoint.table,
                                                 repoint.column, context))));
            }
        }
        // The index rows that name it (its own, the group it heads, its places among other groups' members), the alias
        // recorded with its last key and path, then the identity row, which frees its relative path.
        for (std::size_t i = 0; i < kStatements.size(); ++i) {
            Statement& write = *statements[i];
            write.bindInt(1, id);
            if (kStatements[i] == Sql::InsertAlias) {
                write.bindInt(2, static_cast<std::int64_t>(alias.game.value()))
                    .bindText(3, identity.key.key())
                    .bindText(4, identity.key.relativePath())
                    .bindText(5, m_now)
                    .bindText(6, catalog::toString(alias.reason));
            }
            if (auto written = write.run(); !written) {
                Error error = written.error();
                if (isConstraintFailure(error, "FOREIGN KEY")) {
                    error.message = fmt::format("a state row still names the identity ({})", error.message);
                }
                return propagate(failed(withPrefix(std::move(error), "cannot turn " + context + " into an alias")));
            }
        }
        return ok();
    });
}

Status SqliteCatalogStore::ImportTransaction::putQuickHash(GameId id, const catalog::QuickHash& hash) {
    if (auto open = active(); !open) {
        return open;
    }
    const std::string context = id.toString();
    auto update = statement(Sql::UpdateQuickHash);
    if (!update) {
        return propagate(update.error());
    }
    Statement& row = **update;
    row.bindInt(1, static_cast<std::int64_t>(id.value()));
    if (auto bound = bindQuickHash(row, 2, hash); !bound) {
        row.reset();
        return propagate(withPrefix(bound.error(), "cannot store the quick hash of " + context));
    }
    if (auto stored = row.run(); !stored) {
        return propagate(failed(withPrefix(stored.error(), "cannot store the quick hash of " + context)));
    }
    if (m_state->database.writer().changes() != 1) {
        return makeError(ErrorCode::InvalidArgument, "no identity has this GameId", context);
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::putHistory(const catalog::IdentityEvent& event) {
    if (auto open = active(); !open) {
        return open;
    }
    const std::string context = event.game.toString();
    auto insert = statement(Sql::InsertHistory);
    if (!insert) {
        return propagate(insert.error());
    }
    const auto optionalText = [](const std::string& text) {
        return text.empty() ? std::nullopt : std::optional<std::string_view>(text);
    };
    (*insert)
        ->bindInt(1, static_cast<std::int64_t>(event.game.value()))
        .bindText(2, event.at.empty() ? std::string_view(m_now) : std::string_view(event.at))
        .bindText(3, catalog::toString(event.kind))
        .bindText(4, event.reason ? catalog::toString(*event.reason) : std::string_view())
        .bindOptionalText(5, optionalText(event.fromKey))
        .bindOptionalText(6, optionalText(event.fromPath))
        .bindOptionalText(7, optionalText(event.toKey))
        .bindOptionalText(8, optionalText(event.toPath))
        .bindOptionalInt(9, event.alias ? std::optional<std::int64_t>(static_cast<std::int64_t>(event.alias->value()))
                                        : std::nullopt)
        .bindOptionalInt(10, event.review)
        .bindText(11, event.detail);
    if (auto stored = (*insert)->run(); !stored) {
        Error error = stored.error();
        if (isConstraintFailure(error, "FOREIGN KEY")) {
            error.message = fmt::format("no identity has this GameId, or no review row that id ({})", error.message);
        }
        return propagate(failed(withPrefix(std::move(error), "cannot record the history of " + context)));
    }
    return ok();
}

Expected<std::int64_t> SqliteCatalogStore::ImportTransaction::putReview(const catalog::ReviewRow& review) {
    if (auto open = active(); !open) {
        return propagate(open.error());
    }
    const std::string context = review.game.toString();
    if (review.vanished.empty() ||
        std::any_of(review.candidates.begin(), review.candidates.end(), [](const LookupKey& key) { return key.empty(); })) {
        return makeError(ErrorCode::InvalidArgument, "a review row needs the vanished path and its candidates' paths",
                         context);
    }
    nlohmann::json candidates = nlohmann::json::array();
    for (const LookupKey& candidate : review.candidates) {
        candidates.push_back(candidate.relativePath());
    }
    const auto candidatesText = jsonText(candidates, "the review's candidates");
    if (!candidatesText) {
        return propagate(candidatesText.error());
    }
    auto insert = statement(Sql::InsertReview);
    if (!insert) {
        return propagate(insert.error());
    }
    (*insert)
        ->bindInt(1, static_cast<std::int64_t>(review.game.value()))
        .bindText(2, review.vanished.key())
        .bindText(3, review.vanished.relativePath())
        .bindText(4, *candidatesText)
        .bindText(5, review.reason)
        .bindText(6, m_now);
    if (auto stored = (*insert)->run(); !stored) {
        Error error = stored.error();
        if (isConstraintFailure(error, "FOREIGN KEY")) {
            error.message = fmt::format("no identity has this GameId ({})", error.message);
        }
        return propagate(failed(withPrefix(std::move(error), "cannot store the review row of " + context)));
    }
    return m_state->database.writer().lastInsertRowId();
}

Status SqliteCatalogStore::ImportTransaction::resolveReview(std::int64_t id, catalog::ReviewResolution resolution,
                                                            std::optional<GameId> with) {
    if (auto open = active(); !open) {
        return open;
    }
    const std::string context = fmt::format("review {}", id);
    auto update = statement(Sql::ResolveReview);
    auto state = statement(Sql::SelectReviewState);
    if (!update || !state) {
        return propagate(!update ? update.error() : state.error());
    }
    (*update)
        ->bindInt(1, id)
        .bindText(2, m_now)
        .bindText(3, catalog::toString(resolution))
        .bindOptionalInt(4, with ? std::optional<std::int64_t>(static_cast<std::int64_t>(with->value())) : std::nullopt);
    if (auto stored = (*update)->run(); !stored) {
        return propagate(failed(withPrefix(stored.error(), "cannot resolve the " + context)));
    }
    if (m_state->database.writer().changes() == 1) {
        return ok();
    }
    (*state)->bindInt(1, id);
    const auto open = readSingleRow(**state, [](const Statement& row, bool hasRow) -> Expected<std::optional<bool>> {
        return hasRow ? std::optional<bool>(row.columnInt(0) != 0) : std::nullopt;
    });
    if (!open) {
        return propagate(failed(open.error()));
    }
    if (!open->has_value()) {
        return makeError(ErrorCode::NotFound, "no review row has this id", context);
    }
    return makeError(ErrorCode::Conflict, "the review row was resolved already", context);
}

Status SqliteCatalogStore::ImportTransaction::resolveReviewsOf(GameId game, catalog::ReviewResolution resolution) {
    if (auto open = active(); !open) {
        return open;
    }
    auto update = statement(Sql::ResolveReviewsOf);
    if (!update) {
        return propagate(update.error());
    }
    (*update)
        ->bindInt(1, static_cast<std::int64_t>(game.value()))
        .bindText(2, m_now)
        .bindText(3, catalog::toString(resolution))
        .bindNull(4);
    if (auto stored = (*update)->run(); !stored) {
        return propagate(failed(withPrefix(stored.error(), "cannot resolve the review rows of " + game.toString())));
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::putImportIssues(const LibraryId& library,
                                                              std::span<const catalog::Issue> issues,
                                                              std::span<const SystemName> systems) {
    if (auto open = active(); !open) {
        return open;
    }
    auto clearAll = statement(Sql::DeleteImportIssues);
    auto clearSystem = statement(Sql::DeleteSystemImportIssues);
    auto insert = statement(Sql::InsertImportIssue);
    if (!clearAll || !clearSystem || !insert) {
        return propagate(!clearAll ? clearAll.error() : !clearSystem ? clearSystem.error() : insert.error());
    }
    const auto named = [&](const catalog::Issue& issue) {
        return systems.empty() || std::any_of(systems.begin(), systems.end(), [&](const SystemName& system) {
                   return system.str() == issue.system;
               });
    };
    return asOne([&]() -> Status {
        if (systems.empty()) {
            (*clearAll)->bindText(1, library.str());
            if (auto cleared = (*clearAll)->run(); !cleared) {
                return propagate(failed(cleared.error()));
            }
        }
        for (const SystemName& system : systems) {
            (*clearSystem)->bindText(1, library.str()).bindText(2, system.str());
            if (auto cleared = (*clearSystem)->run(); !cleared) {
                return propagate(failed(cleared.error()));
            }
        }
        for (const catalog::Issue& issue : issues) {
            if (!named(issue)) {
                continue;
            }
            (*insert)
                ->bindText(1, library.str())
                .bindText(2, catalog::toString(issue.severity))
                .bindText(3, issue.code)
                .bindText(4, issue.system)
                .bindText(5, issue.file)
                .bindText(6, issue.reason);
            if (auto stored = (*insert)->run(); !stored) {
                return propagate(failed(withPrefix(stored.error(), "cannot store the import's issues")));
            }
        }
        return ok();
    });
}

Status SqliteCatalogStore::ImportTransaction::recordImport(const catalog::ImportRecord& record) {
    if (auto open = active(); !open) {
        return open;
    }
    nlohmann::json asked = nlohmann::json::array();
    for (const SystemName& system : record.systemsAsked) {
        asked.push_back(system.str());
    }
    nlohmann::json summary = nlohmann::json::array();
    for (const catalog::SystemChanges& changes : record.systems) {
        summary.push_back(systemChangesJson(changes));
    }
    const auto askedText = jsonText(asked, "the systems asked");
    const auto summaryText = jsonText(summary, "the import's figures");
    if (!askedText || !summaryText) {
        return propagate(!askedText ? askedText.error() : summaryText.error());
    }
    auto insert = statement(Sql::InsertImportRecord);
    if (!insert) {
        return propagate(insert.error());
    }
    (*insert)
        ->bindText(1, record.library.str())
        .bindText(2, catalog::toString(record.kind))
        .bindText(3, record.startedAt)
        .bindText(4, record.finishedAt)
        .bindBool(5, record.acceptRemovals)
        .bindText(6, *askedText)
        .bindText(7, *summaryText);
    if (auto stored = (*insert)->run(); !stored) {
        Error error = stored.error();
        if (isConstraintFailure(error, "FOREIGN KEY")) {
            error.message =
                fmt::format("its library '{}' is not in the store ({})", record.library.str(), error.message);
        }
        return propagate(failed(withPrefix(std::move(error), "cannot record the import")));
    }
    // A bounded history: the newest kKeptImportRecords of the library stay.
    auto prune = statement(Sql::PruneImportRecords);
    if (!prune) {
        return propagate(prune.error());
    }
    (*prune)->bindText(1, record.library.str()).bindInt(2, static_cast<std::int64_t>(kKeptImportRecords));
    if (auto pruned = (*prune)->run(); !pruned) {
        return propagate(failed(withPrefix(pruned.error(), "cannot delete the oldest import records")));
    }
    return ok();
}

Status SqliteCatalogStore::ImportTransaction::commit() {
    if (auto open = active(); !open) {
        return open;
    }
    // A suspended search index is rebuilt from the index tables, its triggers created again, before the commit.
    if (m_searchSuspended) {
        if (auto resumed = resumeSearchIndex(m_state->database.writer()); !resumed) {
            finish();
            return propagate(withPrefix(resumed.error(), "cannot rebuild the search index (the import was rolled back)"));
        }
        m_searchSuspended = false;
    }
    // rv_meta says which lookup key version the keys are of, as this import leaves them, and which sort keys.
    if (auto recorded = recordKeyVersion(); !recorded) {
        finish();
        return propagate(withPrefix(recorded.error(), "cannot commit the import (it was rolled back)"));
    }
    if (auto upsert = statement(Sql::UpsertMeta); !upsert) {
        finish();
        return propagate(withPrefix(upsert.error(), "cannot commit the import (it was rolled back)"));
    } else {
        (*upsert)->bindText(1, "sort_key_version").bindText(2, std::to_string(catalog::kSortKeyVersion));
        if (auto stored = (*upsert)->run(); !stored) {
            finish();
            return propagate(withPrefix(stored.error(), "cannot commit the import (it was rolled back)"));
        }
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
    if (m_state == nullptr) {
        return;
    }
    if (m_state->open != nullptr) {
        spdlog::warn("Catalog database: the store went away with its import transaction open: rolled back");
        m_state->open->detach();
    }
    if (m_state->lock != nullptr) {
        m_state->lock->detach();
        m_state->lock = nullptr;
        m_state->database.lock().unlockImport();
    }
}

Expected<std::unique_ptr<SqliteCatalogStore>> SqliteCatalogStore::open(Database& database,
                                                                      const StoreOptions& options) {
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
    return std::unique_ptr<SqliteCatalogStore>(new SqliteCatalogStore(std::make_unique<State>(database, options)));
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

Expected<std::vector<SystemInfo>> SqliteCatalogStore::systems(const LibraryId& library) {
    auto select = m_state->statement(Sql::SelectSystems);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindText(1, library.str());
    std::vector<SystemInfo> systems;
    while (true) {
        const auto next = row.step();
        if (!next) {
            return propagate(next.error());
        }
        if (!*next) {
            return systems;
        }
        auto system = readSystem(row);
        if (!system) {
            return propagate(system.error());
        }
        systems.push_back(std::move(*system));
    }
}

Expected<std::vector<SystemName>> SqliteCatalogStore::identitySystems(const LibraryId& library) {
    auto select = m_state->statement(Sql::SelectIdentitySystems);
    if (!select) {
        return propagate(select.error());
    }
    Statement& row = **select;
    const ScopedReset reset(row);
    row.bindText(1, library.str());
    std::vector<SystemName> systems;
    while (true) {
        const auto next = row.step();
        if (!next) {
            return propagate(next.error());
        }
        if (!*next) {
            return systems;
        }
        auto system = SystemName::parse(row.columnText(0));
        if (!system) {
            return unreadable(fmt::format("an invalid system name ({})", system.error().message),
                              "rv_game_identities");
        }
        systems.push_back(std::move(*system));
    }
}

Expected<std::vector<catalog::IndexedDiscGroup>> SqliteCatalogStore::discGroups(const LibraryId& library) {
    Connection& writer = m_state->database.writer();
    // Three reads (the groups, their members, their references) of one snapshot.
    std::optional<Transaction> snapshot;
    if (!writer.inTransaction()) {
        auto begun = Transaction::begin(writer, Transaction::Mode::Deferred);
        if (!begun) {
            return propagate(begun.error());
        }
        snapshot = std::move(*begun);
    }
    std::vector<catalog::IndexedDiscGroup> groups; // by head GameId while read
    {
        auto select = m_state->statement(Sql::SelectDiscGroups);
        if (!select) {
            return propagate(select.error());
        }
        Statement& row = **select;
        const ScopedReset reset(row);
        row.bindText(1, library.str());
        while (true) {
            const auto next = row.step();
            if (!next) {
                return propagate(next.error());
            }
            if (!*next) {
                break;
            }
            const std::int64_t value = row.columnInt(0);
            const std::string context = fmt::format("idx_disc_groups row {}", value);
            const auto head = GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
            const auto kind =
                readEnum<catalog::DiscGroupKind>(row.columnText(1), catalog::parseDiscGroupKind, "group kind", context);
            const auto size = readUnsigned(row.columnInt(2), "head size", context);
            auto system = SystemName::parse(row.columnText(4));
            auto key = readKey(row.columnText(5), context);
            if (!head || !kind || !size || !system || !key) {
                return !head ? unreadable("a disc group whose head is no GameId", context)
                       : !kind ? propagate(kind.error())
                       : !size ? propagate(size.error())
                       : !system ? unreadable("a disc group of an invalid system", context)
                                 : propagate(key.error());
            }
            catalog::IndexedDiscGroup group;
            group.head = *head;
            group.group.system = std::move(*system);
            group.group.head = std::move(*key);
            group.group.kind = *kind;
            group.group.headSize = *size;
            group.group.headLastWriteTime = row.columnInt(3);
            groups.push_back(std::move(group));
        }
    }
    const auto groupOf = [&](std::int64_t head) -> catalog::IndexedDiscGroup* {
        const auto found =
            std::lower_bound(groups.begin(), groups.end(), head, [](const catalog::IndexedDiscGroup& group, std::int64_t id) {
                return static_cast<std::int64_t>(group.head.value()) < id;
            });
        return found != groups.end() && static_cast<std::int64_t>(found->head.value()) == head ? &*found : nullptr;
    };
    {
        auto select = m_state->statement(Sql::SelectDiscMembers);
        if (!select) {
            return propagate(select.error());
        }
        Statement& row = **select;
        const ScopedReset reset(row);
        row.bindText(1, library.str());
        while (true) {
            const auto next = row.step();
            if (!next) {
                return propagate(next.error());
            }
            if (!*next) {
                break;
            }
            const std::string context = fmt::format("idx_disc_members row {}", row.columnInt(0));
            catalog::IndexedDiscGroup* group = groupOf(row.columnInt(0));
            const std::int64_t value = row.columnInt(1);
            const auto member = GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
            auto key = readKey(row.columnText(2), context);
            if (group == nullptr || !member || !key) {
                return group == nullptr ? unreadable("members of a disc group that is not there", context)
                       : !member       ? unreadable("a disc member that is no GameId", context)
                                       : propagate(key.error());
            }
            group->members.push_back(*member);
            group->group.members.push_back(std::move(*key));
        }
    }
    {
        auto select = m_state->statement(Sql::SelectDiscReferences);
        if (!select) {
            return propagate(select.error());
        }
        Statement& row = **select;
        const ScopedReset reset(row);
        row.bindText(1, library.str());
        while (true) {
            const auto next = row.step();
            if (!next) {
                return propagate(next.error());
            }
            if (!*next) {
                break;
            }
            const std::string context = fmt::format("idx_disc_references row {}", row.columnInt(0));
            catalog::IndexedDiscGroup* group = groupOf(row.columnInt(0));
            if (group == nullptr) {
                continue; // what a head said, kept after its group went: nothing to give
            }
            auto key = readKey(row.columnText(1), context);
            if (!key) {
                return propagate(key.error());
            }
            group->group.references.push_back(std::move(*key));
        }
    }
    // The port's order: by system, then head (the keys computed again, whatever the casing table they were stored with).
    sortByPermutation(groups, [](const catalog::IndexedDiscGroup& a, const catalog::IndexedDiscGroup& b) {
        return std::tie(a.group.system, a.group.head) < std::tie(b.group.system, b.group.head);
    });
    return groups;
}

Expected<std::optional<catalog::ImportRecord>> SqliteCatalogStore::lastImport(const LibraryId& library) {
    auto select = m_state->statement(Sql::SelectLastImport);
    if (!select) {
        return propagate(select.error());
    }
    (*select)->bindText(1, library.str());
    return readSingleRow(
        **select, [&](const Statement& row, bool hasRow) -> Expected<std::optional<catalog::ImportRecord>> {
            if (!hasRow) {
                return std::optional<catalog::ImportRecord>();
            }
            const std::string context = fmt::format("rv_imports row {}", row.columnInt(0));
            catalog::ImportRecord record;
            record.id = row.columnInt(0);
            record.library = library;
            const auto kind =
                readEnum<catalog::ImportKind>(row.columnText(1), catalog::parseImportKind, "import kind", context);
            if (!kind) {
                return propagate(kind.error());
            }
            record.kind = *kind;
            record.startedAt = std::string(row.columnText(2));
            record.finishedAt = std::string(row.columnText(3));
            record.acceptRemovals = row.columnInt(4) != 0;
            try {
                for (const nlohmann::json& name : nlohmann::json::parse(row.columnText(5))) {
                    auto system = SystemName::parse(name.get<std::string>());
                    if (!system) {
                        return unreadable("an import record asked for an invalid system", context);
                    }
                    record.systemsAsked.push_back(std::move(*system));
                }
                for (const nlohmann::json& figures : nlohmann::json::parse(row.columnText(6))) {
                    auto changes = readSystemChanges(figures, context);
                    if (!changes) {
                        return propagate(changes.error());
                    }
                    record.systems.push_back(std::move(*changes));
                }
            } catch (const nlohmann::json::exception&) {
                return unreadable("an import record whose JSON cannot be read", context);
            }
            return std::optional<catalog::ImportRecord>(std::move(record));
        });
}

namespace {

/// Every row `statement` (its parameters bound) returns, each read by `read`, in its order; the statement is reset.
template <class T, class Read>
Expected<std::vector<T>> readRows(Statement& statement, Read read) {
    const ScopedReset reset(statement);
    std::vector<T> rows;
    while (true) {
        const auto next = statement.step();
        if (!next) {
            return propagate(next.error());
        }
        if (!*next) {
            return rows;
        }
        auto row = read(statement);
        if (!row) {
            return propagate(row.error());
        }
        rows.push_back(std::move(*row));
    }
}

} // namespace

Expected<std::vector<catalog::IdentityEvent>> SqliteCatalogStore::history(GameId game) {
    auto select = m_state->statement(Sql::SelectHistory);
    if (!select) {
        return propagate(select.error());
    }
    (*select)->bindInt(1, static_cast<std::int64_t>(game.value()));
    return readRows<catalog::IdentityEvent>(**select, readHistory);
}

Expected<std::vector<catalog::StoredAlias>> SqliteCatalogStore::aliasesOf(GameId game) {
    auto select = m_state->statement(Sql::SelectAliasesOf);
    if (!select) {
        return propagate(select.error());
    }
    (*select)->bindInt(1, static_cast<std::int64_t>(game.value()));
    return readRows<catalog::StoredAlias>(**select, readAlias);
}

Expected<std::optional<catalog::StoredAlias>> SqliteCatalogStore::alias(GameId alias) {
    auto select = m_state->statement(Sql::SelectAlias);
    if (!select) {
        return propagate(select.error());
    }
    (*select)->bindInt(1, static_cast<std::int64_t>(alias.value()));
    return readSingleRow(**select,
                         [](const Statement& row, bool hasRow) -> Expected<std::optional<catalog::StoredAlias>> {
                             if (!hasRow) {
                                 return std::optional<catalog::StoredAlias>();
                             }
                             auto read = readAlias(row);
                             if (!read) {
                                 return propagate(read.error());
                             }
                             return std::optional<catalog::StoredAlias>(std::move(*read));
                         });
}

Expected<std::vector<catalog::StoredReview>> SqliteCatalogStore::reviews(const LibraryId& library, bool openOnly) {
    auto select = m_state->statement(Sql::SelectReviews);
    if (!select) {
        return propagate(select.error());
    }
    (*select)->bindText(1, library.str()).bindBool(2, openOnly);
    return readRows<catalog::StoredReview>(**select, readReview);
}

Expected<std::optional<catalog::StoredReview>> SqliteCatalogStore::review(std::int64_t id) {
    auto select = m_state->statement(Sql::SelectReview);
    if (!select) {
        return propagate(select.error());
    }
    (*select)->bindInt(1, id);
    return readSingleRow(**select,
                         [](const Statement& row, bool hasRow) -> Expected<std::optional<catalog::StoredReview>> {
                             if (!hasRow) {
                                 return std::optional<catalog::StoredReview>();
                             }
                             auto read = readReview(row);
                             if (!read) {
                                 return propagate(read.error());
                             }
                             return std::optional<catalog::StoredReview>(std::move(*read));
                         });
}

Expected<std::unique_ptr<catalog::IImportLock>> SqliteCatalogStore::lockImport(const LibraryId& library) {
    if (m_state->open != nullptr || m_state->lock != nullptr) {
        return makeError(ErrorCode::Conflict, "this store holds the import lock already", library.str());
    }
    // Without waiting: an import of another process gets Conflict at once.
    if (auto locked = m_state->database.lock().lockImport(); !locked) {
        return propagate(locked.error());
    }
    return std::unique_ptr<catalog::IImportLock>(std::make_unique<ImportLock>(*m_state));
}

Expected<std::unique_ptr<catalog::IImportTransaction>> SqliteCatalogStore::beginImport(const LibraryId& library) {
    if (m_state->open != nullptr) {
        return makeError(ErrorCode::Conflict, "this store's import transaction is still open", library.str());
    }
    // The import lock first (unless lockImport() holds it), without waiting: an import of another process gets
    // Conflict at once.
    DatabaseLock& lock = m_state->database.lock();
    const bool ownsLock = m_state->lock == nullptr;
    if (ownsLock) {
        if (auto locked = lock.lockImport(); !locked) {
            return propagate(locked.error());
        }
    }
    auto transaction = Transaction::begin(m_state->database.writer(), Transaction::Mode::Immediate);
    if (!transaction) {
        if (ownsLock) {
            lock.unlockImport();
        }
        return propagate(withPrefix(transaction.error(), "cannot begin the import"));
    }
    auto open =
        std::make_unique<ImportTransaction>(*m_state, std::move(*transaction), library, core::utcNow(), ownsLock);
    m_state->open = open.get();
    spdlog::debug("Catalog database: import of library '{}' begun", library.str());
    return std::unique_ptr<catalog::IImportTransaction>(std::move(open));
}

} // namespace rv::catalog_db
