#pragma once

// SqliteCatalogQuery: rv_catalog's ICatalogQuery (rv/catalog/catalog_query.hpp, query.hpp) on the catalog database
// (ADR-0008 §5), and the other reads of the catalog: its systems with their counts, and everything it holds about one
// game.
//
// A query is materialized in one read, on the connection the query object was given (a reader from
// Database::openReader() on the app's catalog worker, or in retroverse-cli): the GameIds of the entries that pass its
// filters, in its total order. Every read runs on one snapshot: a single statement, or several in one read transaction
// (Transaction::Mode::Deferred), so that a list is never made of two states of the database; on a reader it never waits
// on the writer (WAL: a reader sees the last committed snapshot while a write transaction is open). Statements are
// prepared once, kept, and reset after each use (ScopedReset), so that no read transaction outlives a call.
//
// Filters (catalog::CatalogQuery):
//  - states: the listed entry states; none listed: Game and MissingFile (OutsideSystemFolder matches nothing: such
//    entries are never indexed);
//  - availabilities: Available only, or every availability when includeUnavailable is set (FR-033: unavailable entries
//    are hidden unless asked for), narrowed to the listed ones when any are listed: an unavailable availability listed
//    without includeUnavailable matches nothing;
//  - systems: the listed system names, of any library; none listed: every system;
//  - text, the search (searchPlan()): split into words with the search index's own tokenizer (FTS5's unicode61 with
//    remove_diacritics 2, on a private in-memory database: searchWords()), every word must be a whole token of the
//    title, sort title, developer, publisher, genre, release year or system full name, the last word a prefix of one,
//    in any column and any order; case and diacritics are ignored ("eclair" finds "Éclair", "ECLAIR" and "Eclair"). The
//    words become one quoted FTS5 string each, joined with AND, the last one with '*': whatever the user types (quotes,
//    '*', '-', ':', parentheses, NEAR, OR, NOT, '^', '+') is never FTS5 syntax, only text that the tokenizer splits.
//    Text that is blank, or holds no word (punctuation only), matches every entry. A one-character text (one code point
//    once ASCII white space is trimmed) is a filter without FTS5, which has no index for one-character prefixes: the
//    entries whose sort key starts with that character lower-cased as sort keys are (case ignored, diacritics not: "é"
//    finds "Éclair", "e" does not). Text that is not UTF-8 is InvalidArgument.
//
// Orders (catalog::SortOrder), each total: Title, (sort key, GameId); System, the system's full name folded as sort keys
// are (catalog::sortKeyOf), then its name, then Title's order (games of a system without an idx_systems row sort as a
// system with an empty full name); ReleaseDate, oldest first, undated entries last, then Title's order. Lists are read
// in Title's order from the index idx_games_sort (schema_search.cpp), the other orders made from it by a stable sort.
//
// Errors of every read: Conflict (SQLite reports the database busy or locked, which a reader meets only in rare cases),
// Cancelled (interrupted), ParseError (a damaged file, or a row the catalog cannot read back), IoError and SQLite's
// others, as Statement::step() reports them.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/catalog_query.hpp>
#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/query.hpp>
#include <rv/catalog/system.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog_db {

class Connection;

/// How a search text is matched (SqliteCatalogQuery::searchPlan()).
struct SearchPlan {
    enum class Kind : std::uint8_t {
        Everything, ///< blank text, or text without a word: no filter
        FirstCharacter, ///< one character: entries whose sort key starts with `prefix` (no FTS5)
        Words,          ///< FTS5: every word a token, the last one a prefix (`match` is the FTS5 query)
    };

    Kind kind = Kind::Everything;
    /// FirstCharacter: the character as sort keys hold it (catalog::sortKeyOf of the trimmed text).
    std::string prefix;
    /// Words: the words of the text as the search index's tokenizer gives them (folded: lower case, no diacritics).
    std::vector<std::string> words;
    /// Words: the FTS5 query, one quoted string per word joined with AND, the last one a prefix ('"super" AND "pix"*').
    std::string match;

    bool operator==(const SearchPlan&) const = default;
};

/// A system of the catalog with the number of its indexed entries (SqliteCatalogQuery::systems()).
struct SystemSummary {
    catalog::LibraryId library;
    /// The system as idx_systems holds it; for entries whose system has no idx_systems row, only its name is set.
    catalog::SystemInfo system;
    /// The entries a query lists by default: Game entries that are Available.
    std::size_t games = 0;
    /// Game and MissingFile entries that are not Available: what CatalogQuery::includeUnavailable adds.
    std::size_t unavailable = 0;
    /// Every indexed entry of the system, whatever its state (NotAGame, DiscMember and Folder entries included).
    std::size_t entries = 0;

    bool operator==(const SystemSummary&) const = default;
};

/// Everything the catalog holds about one game (SqliteCatalogQuery::details(), retroverse-cli show).
struct GameDetails {
    catalog::LibraryId library;
    /// Its system as idx_systems holds it; nothing when idx_systems has no row for it.
    std::optional<catalog::SystemInfo> system;
    /// The entry with its media and sort key, as the store wrote it (catalog::ICatalogStore::indexedEntries()).
    catalog::IndexedEntry indexed;

    bool operator==(const GameDetails&) const = default;
};

class SqliteCatalogQuery final : public catalog::ICatalogQuery {
public:
    /// The catalog's queries on `connection`, which it keeps (a reader from Database::openReader(), usually); a private
    /// in-memory database splits search text into words. Used from one thread at a time, as the connection. Errors:
    /// Conflict when the database's schema version is not programSchemaVersion() (older: migrate() it first, which a
    /// reader cannot; newer: written by a newer Retroverse); InvalidArgument for a connection that is not open; SQLite's.
    [[nodiscard]] static Expected<std::unique_ptr<SqliteCatalogQuery>> open(Connection connection);

    SqliteCatalogQuery(const SqliteCatalogQuery&) = delete;
    SqliteCatalogQuery& operator=(const SqliteCatalogQuery&) = delete;
    ~SqliteCatalogQuery() override;

    /// The GameIds of the entries that pass `query`'s filters, in its order (see above), in one read. Errors: text that
    /// is not UTF-8 (InvalidArgument), and those of every read.
    [[nodiscard]] Expected<std::vector<catalog::GameId>> run(const catalog::CatalogQuery& query) override;

    /// The rows of `ids`, in their order (an id listed twice gives two rows), in one statement; an id without an
    /// idx_games row has none. A page of 30 rows takes well under ADR-0008's 5 ms on 100,000 games in Release.
    [[nodiscard]] Expected<std::vector<catalog::CatalogRow>> fetch(std::span<const catalog::GameId> ids) override;

    /// How run() matches `text` (see above). Errors: text that is not UTF-8 (InvalidArgument), SQLite's.
    [[nodiscard]] Expected<SearchPlan> searchPlan(std::string_view text);

    /// The words of `text` as the search index's tokenizer splits and folds them, in order (repeated words kept).
    /// Errors: text that is not UTF-8 (InvalidArgument), SQLite's.
    [[nodiscard]] Expected<std::vector<std::string>> searchWords(std::string_view text);

    /// Every system of the catalog with its counts, in SortOrder::System's order (the full name folded as sort keys
    /// are, the name, the library), systems known only from their entries included; in one read.
    [[nodiscard]] Expected<std::vector<SystemSummary>> systems();

    /// Everything the catalog holds about the game `id`, in one read; nothing when it has no idx_games row.
    [[nodiscard]] Expected<std::optional<GameDetails>> details(catalog::GameId id);

    /// The connection it reads.
    [[nodiscard]] Connection& connection() noexcept;

private:
    struct State;

    explicit SqliteCatalogQuery(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> m_state;
};

} // namespace rv::catalog_db
