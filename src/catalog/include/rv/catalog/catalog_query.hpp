#pragma once

// Port ICatalogQuery (ADR-0008 §5): the catalog's lists, as query.hpp describes them, and the room's bulk snapshot of
// a system (room_entry.hpp, ADR-0010 §2; declared by I3 step 1a, filled by step 2). Implemented on the database by
// rv_catalog_db (SqliteCatalogQuery, I2 step 9) and by in-memory sources in tests and mock-ups (the quick catalog's
// 100,000-entry mock of step 6).

#include <span>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/query.hpp>
#include <rv/catalog/room_entry.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog {

/// Runs queries and fetches rows. An instance is used from one thread at a time (it may keep prepared statements and a
/// read-only connection); the app's main thread may call fetch() for the visible page (within 5 ms for a page of
/// 100,000 games in Release), every other call runs on the catalog worker.
class ICatalogQuery {
public:
    virtual ~ICatalogQuery() = default;

    /// Materializes `query` in one read: the GameIds of every matching entry, in the query's total order. Errors: the
    /// source cannot be read (IoError, Unavailable), the query is malformed (InvalidArgument).
    [[nodiscard]] virtual Expected<std::vector<GameId>> run(const CatalogQuery& query) = 0;

    /// The rows of `ids` (a page of a materialized list), in the order of `ids`. An id that no longer has an entry (an
    /// import removed it after the list was materialized) has no row: callers match rows to ids by CatalogRow::id.
    [[nodiscard]] virtual Expected<std::vector<CatalogRow>> fetch(std::span<const GameId> ids) = 0;

    /// The room's bulk snapshot of `system` (room_entry.hpp, ADR-0010 §2): every entry of the system in one read, on
    /// the catalog worker. Errors: the source cannot be read (IoError, Unavailable), the system is unknown (NotFound).
    /// The default answers Unsupported: SqliteCatalogQuery implements it (I3 step 2), in-memory sources when a test
    /// needs one.
    [[nodiscard]] virtual Expected<RoomSnapshot> roomEntries(const SystemName& system) {
        return makeError(ErrorCode::Unsupported, "this catalog source gives no room snapshot", system.str());
    }
};

} // namespace rv::catalog
