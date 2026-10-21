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

/// A placed game the CRT may play: its rank in the featured order (its section, its title's priority) and its media.
struct PlacedScreen {
    int section = 2;
    world::RoomTitle title;
    ScreenGame game;
};

/// `placed` in the featured order: by section (the display fixtures, the hero band, the spine-out shelves), then by
/// the room's priority.
[[nodiscard]] std::vector<ScreenGame> featuredOrder(std::vector<PlacedScreen> placed) {
    std::sort(placed.begin(), placed.end(), [](const PlacedScreen& a, const PlacedScreen& b) {
        if (a.section != b.section) {
            return a.section < b.section;
        }
        return world::priorityLess(a.title, b.title);
    });
    std::vector<ScreenGame> games;
    games.reserve(placed.size());
    for (PlacedScreen& entry : placed) {
        games.push_back(std::move(entry.game));
    }
    return games;
}

} // namespace

std::vector<ScreenGame> roomScreenGames(const world::WorldPlan& plan, const world::CollectionLayout& layout,
                                        const RoomSnapshotResult& snapshot) {
    const RoomSnapshotResult* const snapshots[] = {&snapshot};
    return roomScreenGames(plan, layout, snapshots);
}

std::vector<ScreenGame> roomScreenGames(const world::WorldPlan& plan, const world::CollectionLayout& layout,
                                        std::span<const RoomSnapshotResult* const> snapshots) {
    // The sections whose media are known (each its own entries by GameId; a game is of one system).
    std::vector<const RoomSnapshotResult*> known;
    for (const RoomSnapshotResult* snapshot : snapshots) {
        if (snapshot != nullptr && !snapshot->error && snapshot->snapshot.mediaKnown && snapshot->mediaRoot) {
            known.push_back(snapshot);
        }
    }
    if (known.empty()) {
        return {};
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

    // The placed games with media, each title's priority gathered from its versions.
    std::map<catalog::GameId, PlacedScreen> placed;
    for (const world::CollectionItem& item : layout.items) {
        const auto media = withMedia.find(item.game);
        if (media == withMedia.end()) {
            continue;
        }
        PlacedScreen entry;
        entry.section = sectionRank(item.section);
        entry.title.game = item.game;
        if (const catalog::RoomEntry* found = findEntry(item.game)) {
            entry.title.sortKey = found->sortKey;
            gather(entry.title, *found);
        }
        entry.game = media->second;
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
    std::vector<PlacedScreen> order;
    order.reserve(placed.size());
    for (auto& [game, entry] : placed) {
        order.push_back(std::move(entry));
    }
    return featuredOrder(std::move(order));
}

std::vector<ScreenGame> roomScreenGames(const world::CollectionLayout& layout, const RoomItemLookup& itemOf,
                                        const std::filesystem::path& mediaRoot) {
    std::vector<PlacedScreen> placed;
    for (const world::CollectionItem& item : layout.items) {
        const RoomItemText* text = itemOf ? itemOf(item.game) : nullptr;
        if (text == nullptr || !text->screen) {
            continue;
        }
        const RoomItemScreen& screen = *text->screen;
        PlacedScreen entry;
        entry.section = sectionRank(item.section);
        entry.title.game = item.game;
        entry.title.sortKey = screen.sortKey;
        entry.title.favorite = screen.favorite;
        entry.title.playCount = screen.playCount;
        entry.title.ratingTenths = screen.ratingTenths;
        entry.game.id = item.game.toString();
        entry.game.media.video = below(mediaRoot, screen.video);
        entry.game.media.screenshot = below(mediaRoot, screen.screenshot);
        entry.game.media.titleScreen = below(mediaRoot, screen.titleScreen);
        entry.game.media.cover = below(mediaRoot, text->coverPath);
        if (entry.game.media == ScreenMedia{}) {
            continue;
        }
        entry.game.lastPlayed = screen.lastPlayed;
        placed.push_back(std::move(entry));
    }
    return featuredOrder(std::move(placed));
}

std::optional<RoomItemScreen> roomItemScreen(const RoomSnapshotResult& snapshot, const catalog::GameId& game,
                                             std::span<const catalog::GameId> versions) {
    const catalog::RoomEntry* entry = entryOf(snapshot.snapshot, game);
    if (entry == nullptr || entry->state != catalog::EntryState::Game) {
        return std::nullopt;
    }
    RoomItemScreen screen;
    const auto paths = std::lower_bound(
        snapshot.mediaPaths.begin(), snapshot.mediaPaths.end(), game,
        [](const esde::RoomMediaPaths& candidate, const catalog::GameId& id) { return candidate.game < id; });
    if (paths != snapshot.mediaPaths.end() && paths->game == game) {
        screen.video = (*paths)[catalog::MediaKind::Video];
        screen.screenshot = (*paths)[catalog::MediaKind::Screenshot];
        screen.titleScreen = (*paths)[catalog::MediaKind::TitleScreen];
    }
    if (entry->lastSession) {
        screen.lastPlayed =
            std::chrono::duration_cast<std::chrono::seconds>(entry->lastSession->time_since_epoch()).count();
    }
    world::RoomTitle title;
    title.sortKey = entry->sortKey;
    gather(title, *entry);
    for (const catalog::GameId& version : versions) {
        if (const catalog::RoomEntry* other = entryOf(snapshot.snapshot, version)) {
            gather(title, *other);
        }
    }
    screen.sortKey = std::move(title.sortKey);
    screen.favorite = title.favorite;
    screen.playCount = title.playCount;
    screen.ratingTenths = title.ratingTenths;
    return screen;
}

} // namespace rv::app
