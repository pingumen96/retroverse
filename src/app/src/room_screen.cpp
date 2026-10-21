#include <rv/app/room_screen.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <utility>

#include <rv/core/file_io.hpp>
#include <rv/world/room_selection.hpp>

namespace rv::app {
namespace {

/// The featured order's sections: the display fixtures, the hero band, the spine-out shelves.
[[nodiscard]] int sectionRank(world::Section section) noexcept {
    switch (section) {
    case world::Section::Featured:
        return 0;
    case world::Section::Band:
        return 1;
    case world::Section::Spine:
        return 2;
    }
    return 2;
}

/// The entry of `game` in `snapshot` (entries by GameId), or nullptr.
[[nodiscard]] const catalog::RoomEntry* entryOf(const catalog::RoomSnapshot& snapshot, const catalog::GameId& game) {
    const auto found =
        std::lower_bound(snapshot.entries.begin(), snapshot.entries.end(), game,
                         [](const catalog::RoomEntry& entry, const catalog::GameId& id) { return entry.id < id; });
    return found != snapshot.entries.end() && found->id == game ? &*found : nullptr;
}

/// Adds `entry`'s priority data to `title` (world::selectRoom's rule: a favourite when one version is, the play counts
/// summed, the best rating).
void gather(world::RoomTitle& title, const catalog::RoomEntry& entry) {
    title.favorite = title.favorite || entry.favorite;
    title.playCount += entry.esdePlayCount + entry.sessionCount;
    if (entry.rating && (!title.ratingTenths || entry.rating->tenths > *title.ratingTenths)) {
        title.ratingTenths = entry.rating->tenths;
    }
}

/// `relative` (a media path) under the media root, exactly as the catalog names it; nothing when it is empty or does
/// not make a path.
[[nodiscard]] std::filesystem::path below(const std::filesystem::path& root, const std::string& relative) {
    if (relative.empty()) {
        return {};
    }
    auto path = core::extendedLengthPathBelow(root, relative);
    return path ? std::move(*path) : std::filesystem::path();
}

} // namespace

std::vector<ScreenGame> roomScreenGames(const world::WorldPlan& plan, const world::CollectionLayout& layout,
                                        const RoomSnapshotResult& snapshot) {
    const RoomSnapshotResult* const snapshots[] = {&snapshot};
    return roomScreenGames(plan, layout, snapshots);
}

std::vector<ScreenGame> roomScreenGames(const world::WorldPlan& plan, const world::CollectionLayout& layout,
                                        std::span<const RoomSnapshotResult* const> snapshots) {
    std::vector<ScreenGame> games;
    // The sections whose media are known (each its own entries by GameId; a game is of one system).
    std::vector<const RoomSnapshotResult*> known;
    for (const RoomSnapshotResult* snapshot : snapshots) {
        if (snapshot != nullptr && !snapshot->error && snapshot->snapshot.mediaKnown && snapshot->mediaRoot) {
            known.push_back(snapshot);
        }
    }
    if (known.empty()) {
        return games;
    }
    const auto findEntry = [&](const catalog::GameId& game) -> const catalog::RoomEntry* {
        for (const RoomSnapshotResult* snapshot : known) {
            if (const catalog::RoomEntry* entry = entryOf(snapshot->snapshot, game)) {
                return entry;
            }
        }
        return nullptr;
    };
    std::map<catalog::GameId, ScreenGame> withMedia;
    for (const RoomSnapshotResult* snapshot : known) {
        const std::filesystem::path& root = *snapshot->mediaRoot;
        for (const esde::RoomMediaPaths& paths : snapshot->mediaPaths) {
            const catalog::RoomEntry* entry = entryOf(snapshot->snapshot, paths.game);
            if (entry == nullptr || entry->state != catalog::EntryState::Game) {
                continue;
            }
            ScreenGame game;
            game.id = paths.game.toString();
            game.media.video = below(root, paths[catalog::MediaKind::Video]);
            game.media.screenshot = below(root, paths[catalog::MediaKind::Screenshot]);
            game.media.titleScreen = below(root, paths[catalog::MediaKind::TitleScreen]);
            game.media.cover = below(root, paths[catalog::MediaKind::Cover]);
            if (game.media == ScreenMedia{}) {
                continue;
            }
            if (entry->lastSession) {
                game.lastPlayed =
                    std::chrono::duration_cast<std::chrono::seconds>(entry->lastSession->time_since_epoch()).count();
            }
            withMedia.emplace(paths.game, std::move(game));
        }
    }

    // The placed games in the featured order, each title's priority gathered from its versions.
    struct Placed {
        int section = 2;
        world::RoomTitle title;
    };
    std::map<catalog::GameId, Placed> placed;
    for (const world::CollectionItem& item : layout.items) {
        Placed entry;
        entry.section = sectionRank(item.section);
        entry.title.game = item.game;
        if (const catalog::RoomEntry* found = findEntry(item.game)) {
            entry.title.sortKey = found->sortKey;
            gather(entry.title, *found);
        }
        placed.emplace(item.game, std::move(entry));
    }
    for (const world::UnplacedGame& unplaced : plan.unplaced) {
        if (unplaced.reason != world::UnplacedReason::DuplicateTitle) {
            continue;
        }
        const auto representative = placed.find(unplaced.representative);
        const catalog::RoomEntry* version = findEntry(unplaced.game);
        if (representative != placed.end() && version != nullptr) {
            gather(representative->second.title, *version);
        }
    }
    std::vector<const Placed*> order;
    order.reserve(placed.size());
    for (const auto& [game, entry] : placed) {
        order.push_back(&entry);
    }
    std::sort(order.begin(), order.end(), [](const Placed* a, const Placed* b) {
        if (a->section != b->section) {
            return a->section < b->section;
        }
        return world::priorityLess(a->title, b->title);
    });
    games.reserve(withMedia.size());
    for (const Placed* entry : order) {
        const auto found = withMedia.find(entry->title.game);
        if (found == withMedia.end()) {
            continue;
        }
        games.push_back(std::move(found->second));
        withMedia.erase(found);
    }
    // The others, by GameId: played when selected.
    for (auto& [game, screen] : withMedia) {
        screen.startCandidate = false;
        games.push_back(std::move(screen));
    }
    return games;
}

} // namespace rv::app
