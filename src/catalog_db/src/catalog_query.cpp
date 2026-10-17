#include <rv/catalog_db/catalog_query.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::catalog_db {

using catalog::Availability;
using catalog::CatalogEntry;
using catalog::CatalogQuery;
using catalog::CatalogRow;
using catalog::EntryState;
using catalog::GameId;
using catalog::IndexedEntry;
using catalog::LibraryId;
using catalog::SortOrder;
using catalog::SystemInfo;
using catalog::SystemName;

namespace {

/// The private in-memory database that splits search text into words: an FTS5 table with the search index's tokenizer
/// (schema_search.cpp: the same options, which a test compares with the index's SQL), and its vocabulary as instances,
/// which lists the tokens of the one row it holds by their position.
constexpr std::string_view kWordsSchema = R"sql(
CREATE VIRTUAL TABLE search_words USING fts5(text, tokenize = 'unicode61 remove_diacritics 2');
CREATE VIRTUAL TABLE search_word_list USING fts5vocab(search_words, instance);
)sql";

/// Statements kept per connection: the queries' SQL varies with their filters and order, a few dozen forms in all.
constexpr std::size_t kStatementCacheLimit = 64;

/// The page cache of the query connection, in KiB (PRAGMA cache_size = -kQueryCacheKiB).
constexpr std::int64_t kQueryCacheKiB = 32 * 1024;

/// The words a search takes (searchPlan()): beyond them, text is refused rather than made into an FTS5 query of as many
/// terms (no title has that many words).
constexpr std::size_t kMaxSearchWords = 64;

/// A value the database holds that the catalog cannot read back: a damaged or foreign row.
tl::unexpected<Error> unreadable(std::string_view what, std::string context) {
    return makeError(ErrorCode::ParseError, fmt::format("the catalog database holds {}", what), std::move(context));
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

Expected<GameId> readGameId(std::int64_t value, std::string_view table) {
    const auto id = GameId::fromValue(value < 0 ? 0 : static_cast<std::uint64_t>(value));
    if (!id) {
        return unreadable(fmt::format("a row whose GameId {} is none", value), std::string(table));
    }
    return *id;
}

/// `values` as a JSON array of strings, which json_each() lists back (the queries' IN lists: names from the catalog's
/// enumerations and system names, bound as one parameter, never written into the SQL).
std::string jsonStrings(const std::vector<std::string_view>& values) {
    nlohmann::json array = nlohmann::json::array();
    for (const std::string_view value : values) {
        array.push_back(std::string(value));
    }
    return array.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

/// `ids` as a JSON array of integers.
std::string jsonIds(std::span<const GameId> ids) {
    std::string json = "[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        json += fmt::format("{}{}", i == 0 ? "" : ",", ids[i].value());
    }
    json += ']';
    return json;
}

std::string jsonIds(std::span<const std::int64_t> ids) {
    std::string json = "[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        json += fmt::format("{}{}", i == 0 ? "" : ",", ids[i]);
    }
    json += ']';
    return json;
}

/// The code points of valid UTF-8 text: its bytes that do not continue a sequence.
std::size_t codePointCount(std::string_view utf8) noexcept {
    return static_cast<std::size_t>(
        std::count_if(utf8.begin(), utf8.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

/// A word as an FTS5 string: in double quotes, a double quote doubled (the tokenizer never leaves one in a token, as
/// it never leaves any character FTS5 reads as syntax: a string holds text only).
std::string ftsString(std::string_view word) {
    std::string quoted = "\"";
    for (const char c : word) {
        quoted += c;
        if (c == '"') {
            quoted += '"';
        }
    }
    quoted += '"';
    return quoted;
}

/// The key that groups a row's library and system (a NUL never appears in either).
void groupKey(std::string& out, std::string_view library, std::string_view system) {
    out.assign(library);
    out += '\0';
    out += system;
}

/// The columns of idx_systems as readSystem() reads them, after the library.
constexpr std::string_view kSystemColumns = "name, full_name, platforms, theme, rom_folder_configured, rom_folder, "
                                            "availability, unavailable_reason, gamelist, alternative_emulator";

/// A system from the columns kSystemColumns, starting at `first`.
Expected<SystemInfo> readSystem(const Statement& row, int first, const std::string& context) {
    SystemInfo system;
    auto name = SystemName::parse(row.columnText(first));
    if (!name) {
        return unreadable(fmt::format("an invalid system name ({})", name.error().message), context);
    }
    system.name = std::move(*name);
    system.fullName = std::string(row.columnText(first + 1));
    const nlohmann::json platforms = nlohmann::json::parse(row.columnText(first + 2), nullptr, false);
    if (!platforms.is_array()) {
        return unreadable("platforms that are not a JSON array", context);
    }
    for (const nlohmann::json& platform : platforms) {
        if (!platform.is_string()) {
            return unreadable("a platform that is not a JSON string", context);
        }
        system.platforms.push_back(platform.get<std::string>());
    }
    system.theme = std::string(row.columnText(first + 3));
    system.romFolderConfigured = std::string(row.columnText(first + 4));
    auto romFolder = core::tryPathFromUtf8(row.columnText(first + 5));
    if (!romFolder) {
        return unreadable("a ROM folder that is not valid UTF-8", context);
    }
    system.romFolder = std::move(*romFolder);
    const auto availability =
        readEnum<Availability>(row.columnText(first + 6), catalog::parseAvailability, "availability", context);
    const auto reason = readEnum<catalog::SourceUnavailableReason>(
        row.columnText(first + 7), catalog::parseSourceUnavailableReason, "unavailability", context);
    const auto gamelist = readEnum<catalog::GamelistState>(row.columnText(first + 8), catalog::parseGamelistState,
                                                           "gamelist state", context);
    if (!availability || !reason || !gamelist) {
        return propagate(!availability ? availability.error() : !reason ? reason.error() : gamelist.error());
    }
    system.availability = *availability;
    system.unavailableReason = *reason;
    system.gamelist = *gamelist;
    system.alternativeEmulator = std::string(row.columnText(first + 9));
    return system;
}

/// The columns of a list row (CatalogRow), as readRow() reads them; `g` is idx_games, `s` its idx_systems row.
constexpr std::string_view kRowColumns =
    "g.id, g.system, s.full_name, g.title, g.state, g.availability, g.release_date, g.developer, g.publisher, "
    "g.genre, g.players_min, g.players_max, g.rating, g.description, g.favorite";

catalog::ReleaseDate readDate(std::int64_t value) noexcept {
    return catalog::ReleaseDate{static_cast<int>(value / 10000), static_cast<int>(value / 100 % 100),
                                static_cast<int>(value % 100)};
}

std::optional<catalog::Players> readPlayers(const Statement& row, int min, int max) {
    if (row.isNull(min)) {
        return std::nullopt;
    }
    catalog::Players players;
    players.min = static_cast<std::uint32_t>(row.columnInt(min));
    players.max =
        row.isNull(max) ? std::nullopt : std::optional<std::uint32_t>(static_cast<std::uint32_t>(row.columnInt(max)));
    return players;
}

Expected<CatalogRow> readRow(const Statement& row) {
    const auto id = readGameId(row.columnInt(0), "idx_games");
    if (!id) {
        return propagate(id.error());
    }
    const std::string context = fmt::format("idx_games row {}", id->value());
    CatalogRow result;
    result.id = *id;
    auto system = SystemName::parse(row.columnText(1));
    if (!system) {
        return unreadable(fmt::format("an invalid system name ({})", system.error().message), context);
    }
    result.system = std::move(*system);
    result.systemFullName = std::string(row.columnText(2));
    result.title = std::string(row.columnText(3));
    const auto state = readEnum<EntryState>(row.columnText(4), catalog::parseEntryState, "entry state", context);
    const auto availability =
        readEnum<Availability>(row.columnText(5), catalog::parseAvailability, "availability", context);
    if (!state || !availability) {
        return propagate(!state ? state.error() : availability.error());
    }
    result.state = *state;
    result.availability = *availability;
    if (!row.isNull(6)) {
        result.releaseDate = readDate(row.columnInt(6));
    }
    result.developer = catalog::displayText(row.columnText(7));
    result.publisher = catalog::displayText(row.columnText(8));
    result.genre = catalog::displayText(row.columnText(9));
    result.players = readPlayers(row, 10, 11);
    if (!row.isNull(12)) {
        result.rating = catalog::Rating{static_cast<int>(row.columnInt(12))};
    }
    result.description = std::string(row.columnText(13));
    result.favorite = row.columnInt(14) != 0;
    return result;
}

/// The columns of idx_games as readEntry() reads them (every one: what the store wrote).
constexpr std::string_view kEntryColumns =
    "id, library, system, relative_path, state, not_a_game_reason, excluding_setting, availability, "
    "unavailable_reason, is_directory, file_size, file_time, file_hidden, gamelist_path, title, sort_key, name, "
    "sort_name, description, developer, publisher, genre, release_date, rating, players_min, players_max, "
    "disc_head_path, alt_emulator, favorite, hidden, kid_game, completed, broken, play_count, play_time, last_played, "
    "raw";

Expected<catalog::LookupKey> readKey(std::string_view relativePath, const std::string& context) {
    auto key = catalog::makeLookupKey(relativePath);
    if (!key) {
        return unreadable(fmt::format("a relative path that makes no lookup key ({})", key.error().message), context);
    }
    return key;
}

/// The [name, text] pairs of a raw values list.
bool readRawElements(const nlohmann::json& list, std::vector<catalog::RawElement>& out) {
    if (!list.is_array()) {
        return false;
    }
    for (const nlohmann::json& pair : list) {
        if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string()) {
            return false;
        }
        out.push_back(catalog::RawElement{pair[0].get<std::string>(), pair[1].get<std::string>()});
    }
    return true;
}

/// The raw ES-DE values as the store writes them: {"unknown": [[name, text], ...], "original": [...]}.
Expected<catalog::RawEsdeValues> readRaw(std::string_view text, const std::string& context) {
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    catalog::RawEsdeValues raw;
    bool readable = json.is_object();
    if (readable) {
        for (const auto& [key, list] : json.items()) {
            if (key == "unknown") {
                readable = readable && readRawElements(list, raw.unknownElements);
            } else if (key == "original") {
                readable = readable && readRawElements(list, raw.originalForms);
            } else {
                readable = false;
            }
        }
    }
    if (!readable) {
        return unreadable("raw ES-DE values that are not a JSON object of [name, text] lists", context);
    }
    return raw;
}

/// An entry from a row of kEntryColumns (its library is column 1, read by the caller), its media left empty.
Expected<IndexedEntry> readEntry(const Statement& row) {
    const auto id = readGameId(row.columnInt(0), "idx_games");
    if (!id) {
        return propagate(id.error());
    }
    const std::string context = fmt::format("idx_games row {}", id->value());
    IndexedEntry indexed;
    indexed.id = *id;
    CatalogEntry& entry = indexed.entry;
    auto system = SystemName::parse(row.columnText(2));
    if (!system) {
        return unreadable(fmt::format("an invalid system name ({})", system.error().message), context);
    }
    entry.system = std::move(*system);
    auto key = readKey(row.columnText(3), context);
    if (!key) {
        return propagate(key.error());
    }
    entry.key = std::move(*key);
    const auto state = readEnum<EntryState>(row.columnText(4), catalog::parseEntryState, "entry state", context);
    const auto reason =
        readEnum<catalog::NotAGameReason>(row.columnText(5), catalog::parseNotAGameReason, "reason", context);
    const auto availability =
        readEnum<Availability>(row.columnText(7), catalog::parseAvailability, "availability", context);
    const auto unavailable = readEnum<catalog::SourceUnavailableReason>(
        row.columnText(8), catalog::parseSourceUnavailableReason, "unavailability", context);
    if (!state || !reason || !availability || !unavailable) {
        return propagate(!state          ? state.error()
                         : !reason       ? reason.error()
                         : !availability ? availability.error()
                                         : unavailable.error());
    }
    entry.state = *state;
    entry.notAGameReason = *reason;
    entry.excludingSetting = std::string(row.columnText(6));
    entry.availability = *availability;
    entry.unavailableReason = *unavailable;
    if (!row.isNull(9)) {
        const std::int64_t size = row.columnInt(10);
        if (size < 0) {
            return unreadable(fmt::format("a negative file size ({})", size), context);
        }
        entry.file = catalog::FileFacts{row.columnInt(9) != 0, static_cast<std::uint64_t>(size), row.columnInt(11),
                                        row.columnInt(12) != 0};
    }
    entry.gamelistPath = std::string(row.columnText(13));
    entry.title = std::string(row.columnText(14));
    indexed.sortKey = std::string(row.columnText(15));
    catalog::GameMetadata& metadata = entry.metadata;
    metadata.name = std::string(row.columnText(16));
    metadata.sortName = std::string(row.columnText(17));
    metadata.description = std::string(row.columnText(18));
    metadata.developer = std::string(row.columnText(19));
    metadata.publisher = std::string(row.columnText(20));
    metadata.genre = std::string(row.columnText(21));
    if (!row.isNull(22)) {
        metadata.releaseDate = readDate(row.columnInt(22));
    }
    if (!row.isNull(23)) {
        metadata.rating = catalog::Rating{static_cast<int>(row.columnInt(23))};
    }
    metadata.players = readPlayers(row, 24, 25);
    if (!row.isNull(26)) {
        auto head = readKey(row.columnText(26), context);
        if (!head) {
            return propagate(head.error());
        }
        entry.discHead = std::move(*head);
    }
    entry.altEmulator = std::string(row.columnText(27));
    catalog::EsdeFlags& flags = entry.flags;
    flags.favorite = row.columnInt(28) != 0;
    flags.hidden = row.columnInt(29) != 0;
    flags.kidGame = row.columnInt(30) != 0;
    flags.completed = row.columnInt(31) != 0;
    flags.broken = row.columnInt(32) != 0;
    const std::int64_t playCount = row.columnInt(33);
    const std::int64_t playTime = row.columnInt(34);
    if (playCount < 0 || playTime < 0) {
        return unreadable("a negative play count or time", context);
    }
    flags.playCount = static_cast<std::uint64_t>(playCount);
    flags.playTimeSeconds = static_cast<std::uint64_t>(playTime);
    if (!row.isNull(35)) {
        const auto lastPlayed = catalog::parseEsdeDateTime(row.columnText(35));
        if (!lastPlayed) {
            return unreadable(fmt::format("a last played time that is none ({})", lastPlayed.error().message), context);
        }
        flags.lastPlayed = *lastPlayed;
    }
    if (!row.isNull(36)) {
        auto raw = readRaw(row.columnText(36), context);
        if (!raw) {
            return propagate(raw.error());
        }
        entry.raw = std::move(*raw);
    }
    return indexed;
}

/// A read of several statements on one snapshot: a read transaction, unless the connection has a transaction open
/// already (the reads of an import's own connection, which see its writes). finish() ends it.
class ReadScope {
public:
    [[nodiscard]] static Expected<ReadScope> begin(Connection& connection) {
        ReadScope scope;
        if (!connection.inTransaction()) {
            auto transaction = Transaction::begin(connection, Transaction::Mode::Deferred);
            if (!transaction) {
                return propagate(transaction.error());
            }
            scope.m_transaction = std::move(*transaction);
        }
        return scope;
    }

    /// Ends the read transaction (a read commits nothing; its end only lets the snapshot go).
    [[nodiscard]] Status finish() {
        if (m_transaction.active()) {
            return m_transaction.commit();
        }
        return ok();
    }

private:
    Transaction m_transaction;
};

/// What a materialized list's rows carry besides their id, for the orders made from Title's.
struct ListRow {
    GameId id;
    std::uint32_t group = 0;                 ///< System: the row's (library, system), an index of the groups
    std::optional<std::int64_t> releaseDate; ///< ReleaseDate
};

/// `column IN ('a', 'b')`, or `column = 'a'`, of names of the catalog's enumerations: constants of [a-z-], never user
/// text, so written into the SQL, where SQLite compares them faster than a bound list.
std::string inList(std::string_view column, const std::vector<std::string_view>& names) {
    if (names.size() == 1) {
        return fmt::format("{} = '{}'", column, names.front());
    }
    std::string sql = fmt::format("{} IN (", column);
    for (std::size_t i = 0; i < names.size(); ++i) {
        sql += fmt::format("{}'{}'", i == 0 ? "" : ", ", names[i]);
    }
    sql += ')';
    return sql;
}

/// A list as State::readList() reads it from the database.
struct ListRead {
    /// The WHERE clause of the filters, over idx_games as g; ?3 is `systemsJson` when it is not empty.
    std::string filters;
    std::string systemsJson;
    SortOrder sort = SortOrder::Title;
    /// FirstCharacter: the sort keys' first character.
    std::optional<std::string> prefix;
    /// A search's matching rows, ascending, looked up one by one; null: the order index read whole.
    const std::vector<std::int64_t>* lookUp = nullptr;
};

/// The list run() read last without a search, kept while the database is unchanged: a search with the same filters
/// and order narrows it instead of reading it again.
struct KeptList {
    /// The filters and the order it was read with.
    std::string key;
    /// PRAGMA data_version when it was read: another connection's commit changes it.
    std::int64_t dataVersion = 0;
    std::vector<GameId> ids;
};

} // namespace

struct SqliteCatalogQuery::State {
    Connection connection;
    QueryOptions options;
    /// The private in-memory database of the words (kWordsSchema).
    Connection words;
    Statement clearWords;
    Statement insertWords;
    Statement selectWords;
    /// The statements of `connection`, by their SQL.
    std::map<std::string, Statement, std::less<>> statements;

    /// The kept statement of `sql`, prepared on first use. Valid until the next call (which may empty the cache): each
    /// read uses its statements one after the other.
    Expected<Statement*> statement(std::string_view sql) {
        if (const auto found = statements.find(sql); found != statements.end()) {
            return &found->second;
        }
        if (statements.size() >= kStatementCacheLimit) {
            statements.clear();
        }
        auto prepared = connection.prepare(sql);
        if (!prepared) {
            return propagate(prepared.error());
        }
        return &statements.emplace(std::string(sql), std::move(*prepared)).first->second;
    }

    /// PRAGMA data_version of the snapshot being read.
    Expected<std::int64_t> dataVersion() {
        auto pragma = statement("PRAGMA data_version");
        if (!pragma) {
            return propagate(pragma.error());
        }
        Statement& version = **pragma;
        const ScopedReset reset(version);
        const auto row = version.step();
        if (!row) {
            return propagate(row.error());
        }
        return *row ? version.columnInt(0) : 0;
    }

    /// The rows that match an FTS5 query, ascending.
    Expected<std::vector<std::int64_t>> matches(std::string_view match) {
        auto prepared = statement("SELECT rowid FROM idx_game_search WHERE idx_game_search MATCH ?1");
        if (!prepared) {
            return propagate(prepared.error());
        }
        Statement& search = **prepared;
        const ScopedReset reset(search);
        search.bindText(1, match);
        std::vector<std::int64_t> rows;
        while (true) {
            const auto row = search.step();
            if (!row) {
                return propagate(row.error());
            }
            if (!*row) {
                break;
            }
            rows.push_back(search.columnInt(0));
        }
        if (!std::is_sorted(rows.begin(), rows.end())) {
            std::sort(rows.begin(), rows.end());
        }
        return rows;
    }

    /// Reads a list in Title's order from the order index (or by looking a search's rows up), and makes the other
    /// orders from it by a stable sort; in the caller's read.
    Expected<std::vector<GameId>> readList(const ListRead& list);

    /// The list without a search last read, if any.
    std::optional<KeptList> kept;
};

Expected<std::vector<GameId>> SqliteCatalogQuery::State::readList(const ListRead& list) {
    std::string sql = "SELECT g.id";
    if (list.sort == SortOrder::System) {
        sql += ", g.library, g.system";
    } else if (list.sort == SortOrder::ReleaseDate) {
        sql += ", g.release_date";
    }
    sql += list.lookUp != nullptr ? " FROM json_each(?4) AS j CROSS JOIN idx_games AS g ON g.id = j.value"
                                  : " FROM idx_games AS g INDEXED BY idx_games_sort";
    sql += " WHERE " + list.filters;
    if (list.prefix) {
        sql += " AND g.sort_key >= ?5 AND g.sort_key < ?6";
    }
    sql += " ORDER BY g.sort_key, g.id";
    auto prepared = statement(sql);
    if (!prepared) {
        return propagate(prepared.error());
    }
    Statement& select = **prepared;
    std::vector<ListRow> rows;
    std::vector<std::pair<std::string, std::string>> groups; // System: (library, system) by group index
    {
        const ScopedReset reset(select);
        if (!list.systemsJson.empty()) {
            select.bindText(3, list.systemsJson);
        }
        if (list.lookUp != nullptr) {
            select.bindText(4, jsonIds(*list.lookUp));
        }
        if (list.prefix) {
            // Every sort key that starts with the prefix, and only those: valid UTF-8 never holds the byte 0xFF.
            select.bindText(5, *list.prefix).bindText(6, *list.prefix + '\xFF');
        }
        std::unordered_map<std::string, std::uint32_t> groupIndex; // looked up only: never iterated
        std::string key;
        while (true) {
            const auto row = select.step();
            if (!row) {
                return propagate(row.error());
            }
            if (!*row) {
                break;
            }
            const auto id = readGameId(select.columnInt(0), "idx_games");
            if (!id) {
                return propagate(id.error());
            }
            ListRow listed{*id, 0, std::nullopt};
            if (list.sort == SortOrder::System) {
                groupKey(key, select.columnText(1), select.columnText(2));
                auto [found, added] = groupIndex.try_emplace(key, static_cast<std::uint32_t>(groups.size()));
                if (added) {
                    groups.emplace_back(select.columnText(1), select.columnText(2));
                }
                listed.group = found->second;
            } else if (list.sort == SortOrder::ReleaseDate) {
                listed.releaseDate = select.columnOptionalInt(1);
            }
            rows.push_back(listed);
        }
    }

    if (list.sort == SortOrder::System && !rows.empty()) {
        // The systems' order: the full name folded as sort keys are, then the name (equal for one system in two
        // libraries, whose rows then keep Title's order together).
        auto names = statement("SELECT library, name, full_name FROM idx_systems");
        if (!names) {
            return propagate(names.error());
        }
        std::map<std::pair<std::string, std::string>, std::string> fullNames;
        {
            Statement& systemRows = **names;
            const ScopedReset reset(systemRows);
            while (true) {
                const auto row = systemRows.step();
                if (!row) {
                    return propagate(row.error());
                }
                if (!*row) {
                    break;
                }
                fullNames.emplace(
                    std::pair<std::string, std::string>(systemRows.columnText(0), systemRows.columnText(1)),
                    systemRows.columnText(2));
            }
        }
        std::vector<std::pair<std::string, std::string>> orderKeys; // (folded full name, name) by group index
        for (const auto& group : groups) {
            const auto found = fullNames.find(group);
            auto folded = catalog::sortKeyOf(found == fullNames.end() ? std::string_view() : found->second);
            if (!folded) {
                return propagate(folded.error());
            }
            orderKeys.emplace_back(std::move(*folded), group.second);
        }
        std::vector<std::uint32_t> byKey(groups.size());
        std::iota(byKey.begin(), byKey.end(), std::uint32_t{0});
        std::sort(byKey.begin(), byKey.end(),
                  [&](std::uint32_t a, std::uint32_t b) { return orderKeys[a] < orderKeys[b]; });
        std::vector<std::uint32_t> rank(groups.size()); // equal keys, equal ranks
        for (std::size_t i = 0; i < byKey.size(); ++i) {
            rank[byKey[i]] = i == 0                                           ? 0
                             : orderKeys[byKey[i]] == orderKeys[byKey[i - 1]] ? rank[byKey[i - 1]]
                                                                              : rank[byKey[i - 1]] + 1;
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [&](const ListRow& a, const ListRow& b) { return rank[a.group] < rank[b.group]; });
    } else if (list.sort == SortOrder::ReleaseDate) {
        std::stable_sort(rows.begin(), rows.end(), [](const ListRow& a, const ListRow& b) {
            if (a.releaseDate.has_value() != b.releaseDate.has_value()) {
                return a.releaseDate.has_value();
            }
            return a.releaseDate.value_or(0) < b.releaseDate.value_or(0);
        });
    }
    std::vector<GameId> ids;
    ids.reserve(rows.size());
    for (const ListRow& row : rows) {
        ids.push_back(row.id);
    }
    return ids;
}

Expected<std::unique_ptr<SqliteCatalogQuery>> SqliteCatalogQuery::open(Connection connection,
                                                                       const QueryOptions& options) {
    if (!connection.isOpen()) {
        return makeError(ErrorCode::InvalidArgument, "the catalog's queries need an open connection");
    }
    const auto version = connection.userVersion();
    if (!version) {
        return propagate(version.error());
    }
    if (*version != programSchemaVersion()) {
        return makeError(ErrorCode::Conflict,
                         *version < programSchemaVersion()
                             ? fmt::format("the catalog database has schema version {}, this program reads version {}: "
                                           "migrate it first",
                                           *version, programSchemaVersion())
                             : fmt::format("the catalog database has schema version {}, written by a newer Retroverse "
                                           "(this program reads version {})",
                                           *version, programSchemaVersion()),
                         core::pathToUtf8Lossy(connection.path()));
    }
    // A page cache that holds the order index and the search index of a large catalog (100,000 games: about 6 and 4
    // MiB), instead of SQLite's 2 MiB, which every list read whole would cycle through.
    if (auto cache = connection.execute(fmt::format("PRAGMA cache_size = -{}", kQueryCacheKiB)); !cache) {
        return propagate(cache.error());
    }
    auto words = Connection::openMemory();
    if (!words) {
        return propagate(words.error());
    }
    if (auto made = words->execute(kWordsSchema); !made) {
        return propagate(made.error());
    }
    auto state = std::make_unique<State>();
    state->connection = std::move(connection);
    state->options = options;
    state->words = std::move(*words);
    auto clear = state->words.prepare("DELETE FROM search_words");
    auto insert = state->words.prepare("INSERT INTO search_words (rowid, text) VALUES (1, ?1)");
    auto select = state->words.prepare("SELECT term FROM search_word_list ORDER BY offset");
    if (!clear || !insert || !select) {
        return propagate(!clear ? clear.error() : !insert ? insert.error() : select.error());
    }
    state->clearWords = std::move(*clear);
    state->insertWords = std::move(*insert);
    state->selectWords = std::move(*select);
    return std::unique_ptr<SqliteCatalogQuery>(new SqliteCatalogQuery(std::move(state)));
}

SqliteCatalogQuery::SqliteCatalogQuery(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}

SqliteCatalogQuery::~SqliteCatalogQuery() = default;

Connection& SqliteCatalogQuery::connection() noexcept {
    return m_state->connection;
}

Expected<std::vector<std::string>> SqliteCatalogQuery::searchWords(std::string_view text) {
    if (!core::isValidUtf8(text)) {
        return makeError(ErrorCode::InvalidArgument, "the search text is not valid UTF-8");
    }
    State& state = *m_state;
    if (auto cleared = state.clearWords.run(); !cleared) {
        return propagate(cleared.error());
    }
    state.insertWords.bindText(1, text);
    if (auto inserted = state.insertWords.run(); !inserted) {
        return propagate(inserted.error());
    }
    std::vector<std::string> words;
    const ScopedReset reset(state.selectWords);
    while (true) {
        const auto row = state.selectWords.step();
        if (!row) {
            return propagate(row.error());
        }
        if (!*row) {
            break;
        }
        words.emplace_back(state.selectWords.columnText(0));
    }
    return words;
}

Expected<SearchPlan> SqliteCatalogQuery::searchPlan(std::string_view text) {
    if (!core::isValidUtf8(text)) {
        return makeError(ErrorCode::InvalidArgument, "the search text is not valid UTF-8");
    }
    SearchPlan plan;
    const std::string_view trimmed = core::trim(text);
    if (trimmed.empty()) {
        return plan;
    }
    if (codePointCount(trimmed) == 1) {
        auto prefix = catalog::sortKeyOf(trimmed);
        if (!prefix) {
            return propagate(prefix.error());
        }
        plan.kind = SearchPlan::Kind::FirstCharacter;
        plan.prefix = std::move(*prefix);
        return plan;
    }
    auto words = searchWords(text);
    if (!words) {
        return propagate(words.error());
    }
    if (words->empty()) {
        return plan;
    }
    if (words->size() > kMaxSearchWords) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("the search text holds {} words, more than the {} a search takes", words->size(),
                                     kMaxSearchWords));
    }
    plan.kind = SearchPlan::Kind::Words;
    plan.words = std::move(*words);
    for (std::size_t i = 0; i < plan.words.size(); ++i) {
        plan.match += fmt::format("{}{}", i == 0 ? "" : " AND ", ftsString(plan.words[i]));
    }
    plan.match += '*';
    return plan;
}

Expected<std::vector<GameId>> SqliteCatalogQuery::run(const CatalogQuery& query) {
    auto plan = searchPlan(query.text);
    if (!plan) {
        return propagate(plan.error());
    }
    // The filters.
    std::vector<std::string_view> states;
    if (query.states.empty()) {
        states = {catalog::toString(EntryState::Game), catalog::toString(EntryState::MissingFile)};
    } else {
        for (const EntryState state : query.states) {
            states.push_back(catalog::toString(state));
        }
    }
    std::vector<std::string_view> availabilities;
    for (const Availability availability :
         {Availability::Available, Availability::SourceUnavailable, Availability::Missing}) {
        const bool letThrough = query.includeUnavailable || availability == Availability::Available;
        const bool listed = query.availabilities.empty() ||
                            std::find(query.availabilities.begin(), query.availabilities.end(), availability) !=
                                query.availabilities.end();
        if (letThrough && listed) {
            availabilities.push_back(catalog::toString(availability));
        }
    }
    if (availabilities.empty()) {
        return std::vector<GameId>{};
    }
    ListRead list;
    list.filters = inList("g.state", states) + " AND " + inList("g.availability", availabilities);
    if (!query.systems.empty()) {
        std::vector<std::string_view> systems;
        for (const SystemName& system : query.systems) {
            systems.push_back(system.str());
        }
        list.filters += " AND g.system IN (SELECT value FROM json_each(?3))";
        list.systemsJson = jsonStrings(systems);
    }
    list.sort = query.sort;
    const std::string key =
        fmt::format("{} ORDER {} SYSTEMS {}", list.filters, catalog::toString(query.sort), list.systemsJson);

    State& state = *m_state;
    auto scope = ReadScope::begin(state.connection);
    if (!scope) {
        return propagate(scope.error());
    }
    const auto version = state.dataVersion();
    if (!version) {
        return propagate(version.error());
    }
    const bool kept = state.kept && state.kept->key == key && state.kept->dataVersion == *version;
    const auto readAndKeep = [&]() -> Status {
        auto read = state.readList(list);
        if (!read) {
            return propagate(read.error());
        }
        state.kept = KeptList{key, *version, std::move(*read)};
        return ok();
    };

    std::vector<GameId> ids;
    switch (plan->kind) {
    case SearchPlan::Kind::Everything:
        if (!kept) {
            if (auto read = readAndKeep(); !read) {
                return propagate(read.error());
            }
        }
        ids = state.kept->ids;
        break;
    case SearchPlan::Kind::FirstCharacter: {
        list.prefix = plan->prefix;
        auto read = state.readList(list);
        if (!read) {
            return propagate(read.error());
        }
        ids = std::move(*read);
        break;
    }
    case SearchPlan::Kind::Words: {
        const auto matches = state.matches(plan->match);
        if (!matches) {
            return propagate(matches.error());
        }
        if (matches->empty()) {
            break;
        }
        if (!kept && matches->size() <= state.options.lookupLimit) {
            // A few rows: looked up one by one.
            list.lookUp = &*matches;
            auto read = state.readList(list);
            if (!read) {
                return propagate(read.error());
            }
            ids = std::move(*read);
            break;
        }
        // The list without the search, narrowed to the rows that match it, keeps its order.
        if (!kept) {
            if (auto read = readAndKeep(); !read) {
                return propagate(read.error());
            }
        }
        for (const GameId id : state.kept->ids) {
            if (std::binary_search(matches->begin(), matches->end(), static_cast<std::int64_t>(id.value()))) {
                ids.push_back(id);
            }
        }
        break;
    }
    }
    if (auto finished = scope->finish(); !finished) {
        return propagate(finished.error());
    }
    return ids;
}

Expected<std::vector<CatalogRow>> SqliteCatalogQuery::fetch(std::span<const GameId> ids) {
    std::vector<CatalogRow> rows;
    if (ids.empty()) {
        return rows;
    }
    // One statement, one snapshot: the ids drive the join in their order (json_each's key is the array index).
    auto fetched = m_state->statement(
        fmt::format("SELECT {} FROM json_each(?1) AS j CROSS JOIN idx_games AS g ON g.id = j.value "
                    "LEFT JOIN idx_systems AS s ON s.library = g.library AND s.name = g.system ORDER BY j.key",
                    kRowColumns));
    if (!fetched) {
        return propagate(fetched.error());
    }
    Statement& statement = **fetched;
    const ScopedReset reset(statement);
    statement.bindText(1, jsonIds(ids));
    rows.reserve(ids.size());
    while (true) {
        const auto row = statement.step();
        if (!row) {
            return propagate(row.error());
        }
        if (!*row) {
            break;
        }
        auto read = readRow(statement);
        if (!read) {
            return propagate(read.error());
        }
        rows.push_back(std::move(*read));
    }
    return rows;
}

Expected<std::vector<SystemSummary>> SqliteCatalogQuery::systems() {
    State& state = *m_state;
    auto scope = ReadScope::begin(state.connection);
    if (!scope) {
        return propagate(scope.error());
    }
    std::map<std::pair<std::string, std::string>, SystemSummary> summaries; // by (library, name)
    {
        auto systems = state.statement(fmt::format("SELECT library, {} FROM idx_systems", kSystemColumns));
        if (!systems) {
            return propagate(systems.error());
        }
        Statement& statement = **systems;
        const ScopedReset reset(statement);
        while (true) {
            const auto row = statement.step();
            if (!row) {
                return propagate(row.error());
            }
            if (!*row) {
                break;
            }
            const std::string context =
                fmt::format("idx_systems row {}/{}", statement.columnText(0), statement.columnText(1));
            auto library = LibraryId::parse(statement.columnText(0));
            if (!library) {
                return unreadable(fmt::format("an invalid library id ({})", library.error().message), context);
            }
            auto system = readSystem(statement, 1, context);
            if (!system) {
                return propagate(system.error());
            }
            std::pair<std::string, std::string> key(library->str(), system->name.str());
            SystemSummary summary;
            summary.library = std::move(*library);
            summary.system = std::move(*system);
            summaries.emplace(std::move(key), std::move(summary));
        }
    }
    {
        // Counted from the order index, which holds the columns counted: no row of idx_games is read.
        auto counts = state.statement("SELECT library, system, state, availability, count(*) "
                                      "FROM idx_games INDEXED BY idx_games_sort GROUP BY library, system, state, "
                                      "availability");
        if (!counts) {
            return propagate(counts.error());
        }
        Statement& statement = **counts;
        const ScopedReset reset(statement);
        while (true) {
            const auto row = statement.step();
            if (!row) {
                return propagate(row.error());
            }
            if (!*row) {
                break;
            }
            std::pair<std::string, std::string> key(statement.columnText(0), statement.columnText(1));
            auto found = summaries.find(key);
            if (found == summaries.end()) {
                // Entries of a system without an idx_systems row: listed by name.
                const std::string context = fmt::format("idx_games of {}/{}", key.first, key.second);
                auto library = LibraryId::parse(key.first);
                auto name = SystemName::parse(key.second);
                if (!library || !name) {
                    return unreadable("an invalid library id or system name", context);
                }
                SystemSummary summary;
                summary.library = std::move(*library);
                summary.system.name = std::move(*name);
                found = summaries.emplace(std::move(key), std::move(summary)).first;
            }
            const std::string_view entryState = statement.columnText(2);
            const std::string_view availability = statement.columnText(3);
            const auto count = static_cast<std::size_t>(std::max<std::int64_t>(statement.columnInt(4), 0));
            const bool listedState = entryState == catalog::toString(EntryState::Game) ||
                                     entryState == catalog::toString(EntryState::MissingFile);
            const bool available = availability == catalog::toString(Availability::Available);
            SystemSummary& summary = found->second;
            if (entryState == catalog::toString(EntryState::Game) && available) {
                summary.games += count;
            } else if (listedState && !available) {
                summary.unavailable += count;
            }
            summary.entries += count;
        }
    }
    if (auto finished = scope->finish(); !finished) {
        return propagate(finished.error());
    }
    // SortOrder::System's order of the systems, then the library.
    std::vector<std::pair<std::string, SystemSummary>> ordered; // (folded full name, summary)
    for (auto& [key, summary] : summaries) {
        auto folded = catalog::sortKeyOf(summary.system.fullName);
        if (!folded) {
            return propagate(folded.error());
        }
        ordered.emplace_back(std::move(*folded), std::move(summary));
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return std::tie(a.first, a.second.system.name, a.second.library) <
               std::tie(b.first, b.second.system.name, b.second.library);
    });
    std::vector<SystemSummary> result;
    result.reserve(ordered.size());
    for (auto& [folded, summary] : ordered) {
        result.push_back(std::move(summary));
    }
    return result;
}

Expected<std::optional<GameDetails>> SqliteCatalogQuery::details(GameId id) {
    State& state = *m_state;
    if (!id.isValid()) {
        return std::optional<GameDetails>{};
    }
    auto scope = ReadScope::begin(state.connection);
    if (!scope) {
        return propagate(scope.error());
    }
    GameDetails details;
    {
        auto game = state.statement(fmt::format("SELECT {} FROM idx_games WHERE id = ?1", kEntryColumns));
        if (!game) {
            return propagate(game.error());
        }
        Statement& statement = **game;
        const ScopedReset reset(statement);
        statement.bindInt(1, static_cast<std::int64_t>(id.value()));
        const auto row = statement.step();
        if (!row) {
            return propagate(row.error());
        }
        if (!*row) {
            if (auto finished = scope->finish(); !finished) {
                return propagate(finished.error());
            }
            return std::optional<GameDetails>{};
        }
        auto library = LibraryId::parse(statement.columnText(1));
        if (!library) {
            return unreadable(fmt::format("an invalid library id ({})", library.error().message),
                              fmt::format("idx_games row {}", id.value()));
        }
        details.library = std::move(*library);
        auto entry = readEntry(statement);
        if (!entry) {
            return propagate(entry.error());
        }
        details.indexed = std::move(*entry);
    }
    {
        auto media = state.statement("SELECT kind, extension FROM idx_media WHERE game = ?1 ORDER BY kind");
        if (!media) {
            return propagate(media.error());
        }
        Statement& statement = **media;
        const ScopedReset reset(statement);
        statement.bindInt(1, static_cast<std::int64_t>(id.value()));
        while (true) {
            const auto row = statement.step();
            if (!row) {
                return propagate(row.error());
            }
            if (!*row) {
                break;
            }
            const std::int64_t kind = statement.columnInt(0);
            if (kind < 0 || kind >= static_cast<std::int64_t>(catalog::kMediaKindCount)) {
                return unreadable(fmt::format("an unknown media kind {}", kind),
                                  fmt::format("idx_media of {}", id.value()));
            }
            details.indexed.media.kinds[static_cast<std::size_t>(kind)] =
                catalog::MediaPresence{true, std::string(statement.columnText(1))};
        }
    }
    {
        auto system = state.statement(
            fmt::format("SELECT library, {} FROM idx_systems WHERE library = ?1 AND name = ?2", kSystemColumns));
        if (!system) {
            return propagate(system.error());
        }
        Statement& statement = **system;
        const ScopedReset reset(statement);
        statement.bindText(1, details.library.str()).bindText(2, details.indexed.entry.system.str());
        const auto row = statement.step();
        if (!row) {
            return propagate(row.error());
        }
        if (*row) {
            auto info = readSystem(
                statement, 1,
                fmt::format("idx_systems row {}/{}", details.library.str(), details.indexed.entry.system.str()));
            if (!info) {
                return propagate(info.error());
            }
            details.system = std::move(*info);
        }
    }
    if (auto finished = scope->finish(); !finished) {
        return propagate(finished.error());
    }
    return std::optional<GameDetails>(std::move(details));
}

} // namespace rv::catalog_db
