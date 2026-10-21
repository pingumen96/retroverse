#pragma once

// The catalog's stamp for the world (I4 step 6b, docs/i4/step-6b.md): a short text that changes whenever what a room
// snapshot of a library reads (SqliteCatalogQuery::roomEntries, aliasTarget) may have changed, so that the app's
// building job can tell, without reading every system's snapshot, that the catalog is the one its stored plan was made
// from. It holds:
//   - the library's newest import (rv_imports' id, AUTOINCREMENT, and when it finished): every committed import, refresh
//     or index rebuild records one, also one that changed nothing, and nothing else writes the index tables;
//   - Retroverse's play sessions that count for a game (roomEntries' rule: no launch test, every outcome but a failure to
//     start or a launch blocked): how many and the latest start;
//   - the identities' aliases and history (written outside imports too: their number and newest row);
//   - the schema's version and the sort keys' version (rv_meta).
// Read-only; one connection, any role.

#include <string>

#include <rv/catalog/ids.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog_db {

/// The stamp of `library` in the catalog `connection` reads (see the file comment): text of the fields above, joined.
/// Errors: those of the catalog (a damaged or foreign database).
[[nodiscard]] Expected<std::string> catalogStamp(Connection& connection, const catalog::LibraryId& library);

} // namespace rv::catalog_db
