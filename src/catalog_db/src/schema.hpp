#pragma once

// The schema of the catalog database (ADR-0008 §5), private to rv_catalog_db. It is split into areas, one per owning
// I2 step and one source each, so that a step adds its tables in its own file:
//   identity (schema_identity.cpp, step 4): the state tables rv_* that hold what only Retroverse knows: rv_meta,
//            rv_libraries, rv_game_identities, rv_identity_history, rv_identity_aliases, rv_identity_reviews,
//            rv_imports (step 11a)
//   index    (schema_index.cpp, step 4): the index tables idx_*, a function of the ES-DE snapshot and the identities,
//            rebuildable at any time: idx_systems, idx_games, idx_media, idx_disc_groups, idx_disc_members,
//            idx_disc_references (step 11a), idx_import_issues
//   search   (schema_search.cpp, step 9): the FTS5 index over idx_games and what the queries need
//   launch   (schema_launch.cpp, step 12): play sessions, per-game and per-system launch choices, launch
//            verifications: rv_play_sessions, rv_launch_game_choices, rv_launch_system_choices, rv_launch_verifications;
//            since I7 step 4 (version 2) the programs chosen for emulators: rv_launch_program_choices
// The program's migration to version v runs every area's steps of version v, in the order above, in one transaction
// (programMigrations(), migrator.cpp): an area may refer to the tables of the areas before it, never to those after it.
// Rules every area keeps: STRICT tables; foreign keys point from idx_* to rv_* (and among rv_*), never into idx_*, and
// none cascades, so that a rebuild of the index never touches the state; game rows take the GameId as INTEGER PRIMARY
// KEY, other ids shown to users are AUTOINCREMENT (never reused); text in UTF-8 compared byte for byte (BINARY), as the
// catalog orders keys; times as UTC ISO-8601 text (core::formatUtcIso8601); enumerations as their toString() names.
// Schema v1 stays open until the M1 gate: steps add to v1 (migrator.hpp).

#include <array>
#include <span>
#include <string_view>

#include <rv/catalog_db/database.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog_db::schema {

/// An area's part of one schema version.
struct AreaStep {
    /// The schema version it belongs to.
    int version = 0;
    /// Statements run in order (Connection::execute), inside the version's migration transaction; may be empty.
    std::string_view sql;
    /// Run after `sql`, in the same transaction, for what SQL alone cannot write (rows of runtime values); may be null.
    Status (*apply)(Connection& connection) = nullptr;
    /// It rebuilds tables (SQLite's procedure, migrator.hpp): its version's migration runs with foreign keys off.
    bool rebuildsTables = false;
};

/// An area: its name and its steps, by version.
struct Area {
    std::string_view name;
    std::span<const AreaStep> steps;
};

/// The state tables (I2 step 4).
[[nodiscard]] Area identityArea();
/// The index tables (I2 step 4).
[[nodiscard]] Area indexArea();
/// The search index (I2 step 9; no steps before it).
[[nodiscard]] Area searchArea();
/// Launch choices, verifications and play sessions (I2 step 12; no steps before it).
[[nodiscard]] Area launchArea();

/// Every area, in the order their steps run within a version.
[[nodiscard]] inline std::array<Area, 4> areas() {
    return {identityArea(), indexArea(), searchArea(), launchArea()};
}

} // namespace rv::catalog_db::schema
