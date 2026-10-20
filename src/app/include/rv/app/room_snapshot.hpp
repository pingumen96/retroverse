#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The room's snapshot on the catalog worker (I3 step 10b; ADR-0010 §12, ADR-0011 §6): the database part of making a
// room, one job of the catalog worker (CatalogWorker::roomSnapshot) that runs, in this order, the system's bulk
// snapshot on the worker's reader (ICatalogQuery::roomEntries: one read), the media root's check against a fresh
// resolver (esde::checkRoomMedia: the root's own listing only, so that an unplugged drive is seen at every snapshot;
// the media root resolved from ES-DE's settings as they are now), the media paths derived from the stored presence
// (esde::roomMediaPaths: no folder listed), and, for the placed games of the plan the world holds that the snapshot no
// longer lists, the aliases the catalog records (ICatalogStore::alias, ADR-0008 §4: what world::update hands a slot
// over to). Everything else (the selection, the generation or the update, the plan's write, the scene) runs on the
// worker pool (world_build.hpp), never here, so that the quick catalog's lists and a launch's resolution never queue
// behind it.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rv/catalog/catalog_query.hpp>
#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/room_entry.hpp>
#include <rv/core/expected.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/world/incremental.hpp>

namespace rv::app {

/// What the world asks of the catalog worker.
struct RoomSnapshotRequest {
    /// The world's request (answered with it, so that an answer to an older request is recognized).
    std::uint64_t serial = 0;
    catalog::SystemName system;
    /// The games the plan the world holds places (none without a plan): those the snapshot lacks are asked for their
    /// aliases.
    std::vector<catalog::GameId> placedGames;
};

/// The worker's answer (CatalogResults::room).
struct RoomSnapshotResult {
    std::uint64_t serial = 0;
    catalog::SystemName system;
    /// The catalog's library (library.json's, as the catalog opened with it).
    catalog::LibraryId library;
    /// The snapshot could not be read: NotFound for a system the catalog lacks, the catalog's errors otherwise; the
    /// rest of the result says nothing then.
    std::optional<Error> error;
    /// mediaKnown set by esde::checkRoomMedia.
    catalog::RoomSnapshot snapshot;
    /// esde::roomMediaPaths(snapshot), by GameId.
    std::vector<esde::RoomMediaPaths> mediaPaths;
    /// The media root the paths are relative to (resolved from ES-DE's settings and library.json); none when it could
    /// not be resolved.
    std::optional<std::filesystem::path> mediaRoot;
    /// The system's full name (its <fullname>, else its name): the generated labels' system line.
    std::string systemFullName;
    /// The system's ES-DE platform tags (CatalogSystem::platforms; set by the worker): world::classify()'s fallback
    /// for a system systems.json does not name, so that the app classifies a custom system as the CLI does.
    std::vector<std::string> platformTags;
    /// The aliases of the requested placed games the snapshot lacks, in GameId order (world::UpdateInput::aliases).
    std::vector<world::GameAlias> aliases;
    /// An alias could not be read: the update then runs without aliases (a game gone keeps its slot, ADR-0010 §10).
    std::optional<Error> aliasError;
    /// Milliseconds on the worker: the bulk query, the media root's check and the paths, the aliases, the whole job.
    double queryMs = 0.0;
    double mediaMs = 0.0;
    double aliasMs = 0.0;
    double milliseconds = 0.0;
    /// When the job was done (the run report's times count from the process's creation).
    std::chrono::system_clock::time_point at;
};

/// The job (on the catalog worker's thread, or a test's): the snapshot of `request.system` from `query`, its media
/// checked against `mediaRoot` with a resolver of its own, its paths derived, and the aliases of the requested placed
/// games it lacks from `store` (none asked when `store` is null). Never fails as a whole: an error is in the result.
[[nodiscard]] RoomSnapshotResult takeRoomSnapshot(catalog::ICatalogQuery& query, catalog::ICatalogStore* store,
                                                  const esde::ResolvedRoot& mediaRoot,
                                                  const catalog::LibraryId& library, std::string systemFullName,
                                                  const RoomSnapshotRequest& request);

/// The cover's media path of `game` among `paths` (sorted by GameId, as roomMediaPaths gives them); empty when it has
/// none.
[[nodiscard]] std::string coverPathOf(std::span<const esde::RoomMediaPaths> paths, const catalog::GameId& game);

} // namespace rv::app
