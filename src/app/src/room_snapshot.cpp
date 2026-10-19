#include <rv/app/room_snapshot.hpp>

#include <algorithm>
#include <utility>

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
