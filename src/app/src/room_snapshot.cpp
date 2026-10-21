#include <rv/app/room_snapshot.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog_db/catalog_stamp.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/esde/settings.hpp>

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

} // namespace

RoomSnapshotResult takeRoomSnapshot(catalog::ICatalogQuery& query, catalog::ICatalogStore* store,
                                    const esde::ResolvedRoot& mediaRoot, const catalog::LibraryId& library,
                                    std::string systemFullName, const RoomSnapshotRequest& request) {
    const Clock::time_point start = Clock::now();
    RoomSnapshotResult result;
    result.serial = request.serial;
    result.system = request.system;
    result.library = library;
    result.systemFullName = std::move(systemFullName);
    if (result.systemFullName.empty()) {
        result.systemFullName = request.system.str();
    }
    auto snapshot = query.roomEntries(request.system);
    result.queryMs = millisecondsSince(start);
    if (!snapshot) {
        result.error = snapshot.error();
        result.milliseconds = millisecondsSince(start);
        result.at = std::chrono::system_clock::now();
        return result;
    }
    result.snapshot = std::move(*snapshot);

    // A resolver of its own for every snapshot: an unplugged drive is seen now, not at the catalog's opening.
    const Clock::time_point media = Clock::now();
    esde::EsdeMediaResolver resolver(mediaRoot);
    esde::checkRoomMedia(result.snapshot, resolver);
    result.mediaRoot = resolver.mediaRoot();
    result.mediaPaths = esde::roomMediaPaths(result.snapshot);
    result.mediaMs = millisecondsSince(media);

    const Clock::time_point aliases = Clock::now();
    if (store != nullptr && !request.placedGames.empty()) {
        std::vector<catalog::GameId> placed = request.placedGames;
        std::sort(placed.begin(), placed.end());
        placed.erase(std::unique(placed.begin(), placed.end()), placed.end());
        // The snapshot's entries come by GameId: a game it lacks is found by a binary search.
        const auto listed = [&](const catalog::GameId& game) {
            const auto found = std::lower_bound(
                result.snapshot.entries.begin(), result.snapshot.entries.end(), game,
                [](const catalog::RoomEntry& entry, const catalog::GameId& id) { return entry.id < id; });
            return found != result.snapshot.entries.end() && found->id == game;
        };
        for (const catalog::GameId& game : placed) {
            if (listed(game)) {
                continue;
            }
            auto alias = store->alias(game);
            if (!alias) {
                result.aliasError = alias.error();
                result.aliases.clear();
                break;
            }
            if (*alias) {
                result.aliases.push_back(world::GameAlias{game, (*alias)->game});
            }
        }
    }
    result.aliasMs = millisecondsSince(aliases);
    result.milliseconds = millisecondsSince(start);
    result.at = std::chrono::system_clock::now();
    return result;
}

const RoomSnapshotResult* BuildingSnapshot::find(const catalog::SystemName& system) const noexcept {
    for (const RoomSnapshotResult& result : systems) {
        if (result.system == system) {
            return &result;
        }
    }
    return nullptr;
}

bool BuildingSnapshot::mediaKnown() const noexcept {
    bool any = false;
    for (const RoomSnapshotResult& result : systems) {
        if (result.error) {
            continue;
        }
        if (!result.snapshot.mediaKnown) {
            return false;
        }
        any = true;
    }
    return any;
}

BuildingSnapshot takeBuildingSnapshot(WorldCatalogReader& reader, std::span<const catalog::GameId> placedGames,
                                      const SystemFilter& wanted) {
    const Clock::time_point start = Clock::now();
    BuildingSnapshot building;
    building.library = reader.profile.id;
    const auto finish = [&]() -> BuildingSnapshot {
        building.milliseconds = millisecondsSince(start);
        building.at = std::chrono::system_clock::now();
        return std::move(building);
    };
    if (!reader.query) {
        building.error = Error{ErrorCode::Unavailable, "the catalog has no reader for the building", {}};
        return finish();
    }
    catalog_db::SqliteCatalogQuery& query = *reader.query;
    // One read transaction: every snapshot and alias reads the same state of the catalog (ADR-0013 §9).
    catalog_db::Transaction transaction;
    if (!query.connection().inTransaction()) {
        auto begun = catalog_db::Transaction::begin(query.connection(), catalog_db::Transaction::Mode::Deferred);
        if (!begun) {
            building.error = begun.error();
            return finish();
        }
        transaction = std::move(*begun);
    }
    auto summaries = query.systems();
    if (!summaries) {
        building.error = summaries.error();
        return finish();
    }
    // What the snapshots below are read from (I4 step 6b): the building stamp's catalog part, in the same transaction.
    if (auto stamp = catalog_db::catalogStamp(query.connection(), reader.profile.id)) {
        building.catalogStamp = std::move(*stamp);
    } else {
        spdlog::warn("Building snapshot: the catalog's stamp cannot be read: {}", stamp.error().toString());
    }
    // ES-DE's settings as they are now: the media root it would use (library.json's override over them).
    const auto settings = esde::readSettings(reader.profile.esdeHome);
    const esde::ResolvedRoot mediaRoot =
        esde::resolveMediaRoot(settings ? *settings : esde::EsdeSettings{}, reader.profile);
    std::set<catalog::GameId> listed;
    std::vector<std::string> roomless;
    for (const catalog_db::SystemSummary& summary : *summaries) {
        if (summary.library != reader.profile.id) {
            continue;
        }
        if (wanted && !wanted(summary.system.name, summary.system.platforms)) {
            roomless.push_back(summary.system.name.str());
            continue;
        }
        RoomSnapshotRequest request;
        request.system = summary.system.name;
        RoomSnapshotResult result =
            takeRoomSnapshot(query, nullptr, mediaRoot, reader.profile.id, summary.system.fullName, request);
        result.platformTags = summary.system.platforms;
        for (const catalog::RoomEntry& entry : result.snapshot.entries) {
            listed.insert(entry.id);
        }
        building.systems.push_back(std::move(result));
    }
    // Where the read's time went (I4 step 6b): the bulk queries and the media checks, the slowest system.
    double queryMs = 0.0;
    double mediaMs = 0.0;
    std::size_t entries = 0;
    const RoomSnapshotResult* slowest = nullptr;
    for (const RoomSnapshotResult& result : building.systems) {
        queryMs += result.queryMs;
        mediaMs += result.mediaMs;
        entries += result.snapshot.entries.size();
        if (slowest == nullptr || result.milliseconds > slowest->milliseconds) {
            slowest = &result;
        }
    }
    spdlog::info("Building snapshot: {} system(s), {} entries: the queries {:.0f} ms, the media {:.0f} ms{}",
                 building.systems.size(), entries, queryMs, mediaMs,
                 slowest != nullptr ? fmt::format("; the slowest {} ({} entries) {:.0f} ms", slowest->system.str(),
                                                  slowest->snapshot.entries.size(), slowest->milliseconds)
                                    : std::string());
    // The systems left out of the building because they give no room (classify(): excluded, no family), as
    // `worldplan update` notes them: in one line, the first kListedRoomless by name.
    if (!roomless.empty()) {
        constexpr std::size_t kListedRoomless = 24;
        std::sort(roomless.begin(), roomless.end());
        const std::size_t listedCount = std::min(roomless.size(), kListedRoomless);
        std::string names;
        for (std::size_t index = 0; index < listedCount; ++index) {
            names += (index == 0 ? "" : ", ") + roomless[index];
        }
        spdlog::info("World: {} system(s) of the catalog left out of the building, no room: {}{}", roomless.size(),
                     names,
                     roomless.size() > listedCount ? fmt::format(" and {} more", roomless.size() - listedCount)
                                                   : std::string());
    }
    std::sort(building.systems.begin(), building.systems.end(),
              [](const RoomSnapshotResult& a, const RoomSnapshotResult& b) { return a.system < b.system; });
    std::set<catalog::GameId> missing;
    for (const catalog::GameId& game : placedGames) {
        if (!listed.contains(game)) {
            missing.insert(game);
        }
    }
    for (const catalog::GameId& game : missing) {
        auto alias = query.aliasTarget(game);
        if (!alias) {
            building.aliasError = alias.error();
            building.aliases.clear();
            break;
        }
        if (*alias) {
            building.aliases.push_back(world::GameAlias{game, **alias});
        }
    }
    if (transaction.active()) {
        if (Status ended = transaction.commit(); !ended) {
            spdlog::warn("Building snapshot: the read transaction did not end cleanly: {}", ended.error().toString());
        }
    }
    return finish();
}

std::string coverPathOf(std::span<const esde::RoomMediaPaths> paths, const catalog::GameId& game) {
    const auto found =
        std::lower_bound(paths.begin(), paths.end(), game,
                         [](const esde::RoomMediaPaths& entry, const catalog::GameId& id) { return entry.game < id; });
    if (found == paths.end() || found->game != game) {
        return {};
    }
    return (*found)[catalog::MediaKind::Cover];
}

} // namespace rv::app
