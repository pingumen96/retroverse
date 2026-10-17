#pragma once

// The search index during bulk writes (I2 step 9, ADR-0008 §5). The search area's triggers (schema_search.cpp) keep the
// FTS5 index in step with idx_games and idx_systems row by row. FTS5 holds the terms of the rows written in a
// transaction in memory, and writes them out as a new segment of the index whenever a statement opens a savepoint: every
// statement that writes an index row does (its triggers may fail half-way), so a transaction that writes rows one by
// one, as the store does, makes a segment per row, which FTS5 then merges again and again: 100,000 rows written take two
// to three times as long as without the triggers, where tokenizing every row of the catalog once from the index tables
// takes under 2 s (docs/i2/step-9.md has the measurements).
//
// For a bulk write (a first import, `db rebuild-index`: rows written in the thousands), suspendSearchIndex() drops the
// search triggers inside the writer's transaction, and resumeSearchIndex() rebuilds the index from the index tables
// (FTS5's 'rebuild') and creates the triggers again, before the transaction commits. DDL is transactional in SQLite: a
// transaction rolled back keeps the triggers and the index as they were, and readers keep reading the last commit
// throughout. A transaction committed between the two would leave the index behind its tables and the triggers missing,
// which `db check` reports (the schema differs from the program's; FTS5's integrity check fails): resumeSearchIndex() in
// a later transaction repairs it. Statements prepared on the connection before are prepared again by SQLite when they
// next run (the schema changed).

#include <rv/core/expected.hpp>

namespace rv::catalog_db {

class Connection;

/// Drops the search area's triggers, inside the write transaction open on `connection` (the writer): the rows written
/// after it are not indexed until resumeSearchIndex(). Nothing when they are dropped already. Errors: InvalidArgument
/// when no transaction is open on the connection (the triggers would be gone for good); SQLite's.
[[nodiscard]] Status suspendSearchIndex(Connection& connection);

/// Rebuilds the search index from idx_games and idx_systems (every row tokenized once) and creates the search triggers
/// again, inside the write transaction open on `connection`. Also a repair, with or without a suspension before it.
/// Errors: InvalidArgument when no transaction is open on the connection; SQLite's.
[[nodiscard]] Status resumeSearchIndex(Connection& connection);

/// True when every search trigger is in place: the index follows the writes (false between suspendSearchIndex() and
/// resumeSearchIndex()).
[[nodiscard]] Expected<bool> searchIndexFollowsWrites(Connection& connection);

} // namespace rv::catalog_db
