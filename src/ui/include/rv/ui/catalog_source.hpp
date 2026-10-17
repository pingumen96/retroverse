#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The games the quick catalog lists, as the UI sees them: display rows fetched by page from a source. rv_ui never links
// rv_catalog: rv_app adapts the catalog's ICatalogQuery (rv/catalog/catalog_query.hpp) to CatalogSource, whose paging
// contract mirrors it (ADR-0008 §5), and tests and mock-ups use InMemoryCatalogSource.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::ui {

/// A game as the quick catalog shows it (specification §13.2: title, platform, year, developer, publisher, genre).
/// Catalog data, shown as it comes (style §17.2).
struct CatalogEntry {
    /// Stable and unique: the GameId text form of the catalog ('g' and 16 hexadecimal digits), or any isValidEntryId
    /// name in tests and mock-ups.
    std::string id;
    std::string title;
    /// Display name of its platform.
    std::string platform;
    /// Year of release, 0 when unknown.
    int year = 0;
    std::string developer;
    std::string publisher;
    std::string genre;
    /// Number of players as the catalog writes it ("1", "1-2").
    std::string players;
    std::string description;
    /// What keeps the game from starting, as a key the documents label and style (quick_catalog::kState*: the
    /// catalog's availability and launch readiness, ADR-0008 §4, §8); empty for a game ready to start. The text is the
    /// documents' (Italian strings stay in assets/ui), so a key they do not name shows no label.
    std::string state;
    /// The launch readiness behind `state`, when the source knows it (the ES-DE catalog, ADR-0008 §8: one of
    /// quick_catalog::kReadinessKeys, "ready" included); empty when it does not (mock-ups), and the card then labels
    /// `state` alone.
    std::string readiness;
    /// The first reason the game cannot start (a blocker code: one of quick_catalog::kBlockerCodes, which the card
    /// labels) and that reason's own sentence, shown as it comes on the card's "Dettagli" line; empty for a game that
    /// can start.
    std::string blocker;
    std::string blockerDetail;
    /// The command or profile the game starts with returned normally from a launch of its system (ADR-0008 §8:
    /// "verified" is shown).
    bool verified = false;

    bool operator==(const CatalogEntry&) const = default;
};

/// True when `id` is a usable CatalogEntry::id: 1 to 64 ASCII letters, digits, '-' or '_'.
[[nodiscard]] bool isValidEntryId(std::string_view id) noexcept;

/// A filter chip: every game (empty key) or one platform.
struct PlatformFilter {
    std::string key;       ///< empty for every game, else the platform's key (the in-memory source: its display name)
    std::string label;     ///< what the chip shows; the documents name the chip of every game themselves ("Tutte")
    std::size_t count = 0; ///< entries of the catalog it lets through, the search aside

    bool operator==(const PlatformFilter&) const = default;
};

/// What a CatalogSource lists: the quick catalog's search text and platform filter.
struct CatalogSourceQuery {
    /// What the user typed. How words match is the source's (the catalog's FTS5 index, ADR-0008 §5; the in-memory
    /// source's ASCII words); blank text matches every entry.
    std::string text;
    /// The key of a platform (PlatformFilter::key) whose entries match; empty: every platform.
    std::string platform;

    bool operator==(const CatalogSourceQuery&) const = default;
};

/// Where a query of a CatalogSource stands.
enum class QueryState : std::uint8_t {
    /// Its list is materialized: count(), rows() and position() read it.
    Ready,
    /// It runs elsewhere (the catalog worker, ADR-0008 §5): count(), rows() and position() still read the previous list
    /// until poll() switches to the new one.
    Pending,
};

/// The paged games of the quick catalog (the UI's side of rv_catalog's ICatalogQuery, ADR-0008 §5). query()
/// materializes the ordered list of the entries a query matches in one read; count() is its size, rows() fetches the
/// display rows of a slice of it by position and position() finds an entry's id in it, so that a virtualized view keeps
/// state for the rows it shows only, jumps anywhere, and restores its selection by id when the list is materialized
/// again (another search, a filter, an import that changed the catalog). The order is total and the source's (the
/// catalog's: the sort order's key, then the GameId).
///
/// Called from the UI's thread only, and never blocking it: platforms(), count(), rows() and position() answer at once
/// (a source over the database fetches a page within ADR-0008's 5 ms for 100,000 games, keeps the platforms it read
/// with the last import and an index of the list it materialized); query() may hand the materialization to the catalog
/// worker, where ICatalogQuery::run() runs, and answer Pending; poll() then collects the list. The list the UI reads
/// changes only within query() and poll(), never between two calls.
class CatalogSource {
public:
    virtual ~CatalogSource() = default;

    /// The platforms of the catalog, one filter chip each, in the order the chips show them, with the entries each
    /// holds (search and filter aside). Errors: the source cannot be read (IoError, Unavailable).
    [[nodiscard]] virtual Expected<std::vector<PlatformFilter>> platforms() = 0;

    /// Materializes `query`, the list count(), rows() and position() read from then on: at once (Ready), or on the
    /// catalog worker (Pending: the previous list stays readable until poll() switches to the new one). A query asked
    /// while another one is pending replaces it: only the latest query's list is ever switched to. Errors: the source
    /// cannot be read (IoError, Unavailable), the query names a platform the source does not have (InvalidArgument);
    /// the list is then empty.
    [[nodiscard]] virtual Expected<QueryState> query(const CatalogSourceQuery& query) = 0;

    /// Collects a pending query: Ready once its list has replaced the previous one, which happens within this call;
    /// Pending while it runs; Ready at once when no query is pending. Errors: the pending query failed (as query());
    /// the list is then empty.
    [[nodiscard]] virtual Expected<QueryState> poll() = 0;

    /// Entries in the list.
    [[nodiscard]] virtual std::size_t count() const noexcept = 0;

    /// Replaces `out` with the rows of positions [first, first + count) of the list, cut at its end (none from count()
    /// on): one row per position, in list order, reusing out's storage. A position whose entry is gone (an import
    /// removed it after the list was materialized) gives a row with its id and nothing else, until the list is
    /// materialized again. Errors: the source cannot be read (IoError, Unavailable); `out` is then empty.
    [[nodiscard]] virtual Status rows(std::size_t first, std::size_t count, std::vector<CatalogEntry>& out) = 0;

    /// The position of the entry `id` in the list; nothing when the list does not hold it.
    [[nodiscard]] virtual std::optional<std::size_t> position(std::string_view id) const = 0;
};

} // namespace rv::ui
