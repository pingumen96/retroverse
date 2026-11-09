#include <rv/app/room_last_played.hpp>

#include <algorithm>
#include <utility>

namespace rv::app {
namespace {

/// The proxy's one cluster (a whole fixture's level).
constexpr std::uint32_t kLastPlayedCluster = world::kWholeFixtureLevel;

} // namespace

std::optional<catalog::GameId> lastPlayedGame(const world::CollectionLayout& layout,
                                              const std::function<const RoomItemText*(const catalog::GameId&)>& textOf,
                                              const std::map<catalog::GameId, std::int64_t>* overrides) {
    std::optional<catalog::GameId> best;
    std::int64_t bestTime = 0;
    for (const world::CollectionItem& item : layout.items) {
        std::optional<std::int64_t> played;
        if (overrides != nullptr) {
            if (const auto found = overrides->find(item.game); found != overrides->end()) {
                played = found->second;
            }
        } else if (const RoomItemText* text = textOf ? textOf(item.game) : nullptr; text != nullptr && text->screen) {
            played = text->screen->lastPlayed;
        }
        if (!played) {
            continue;
        }
        if (!best || *played > bestTime || (*played == bestTime && item.game < *best)) {
            best = item.game;
            bestTime = *played;
        }
    }
    return best;
}

glm::mat4 lastPlayedTransform(const world::CollectionItem& item, const ConsoleSocket& socket) {
    const float scale = insertedScale(itemExtents(item), socket.opening);
    return insertedItemTransform(item.transform, socket, scale, 1.0f);
}

std::optional<world::CollectionLayout> lastPlayedLayout(const world::CollectionLayout& room,
                                                        const catalog::GameId& game, const ConsoleSocket& socket) {
    const auto found = std::find_if(room.items.begin(), room.items.end(),
                                    [&](const world::CollectionItem& item) { return item.game == game; });
    if (found == room.items.end()) {
        return std::nullopt;
    }
    const auto index = static_cast<std::uint32_t>(found - room.items.begin());
    const auto group = std::find_if(room.groups.begin(), room.groups.end(), [&](const world::CollectionGroup& g) {
        return index >= g.firstItem && index < g.firstItem + g.itemCount;
    });
    if (group == room.groups.end()) {
        return std::nullopt;
    }
    world::CollectionItem item = *found;
    // The slot frame keeps its relation to the item (the prefab's pose in the slot), so that the box and the extents
    // follow the item into the socket.
    const glm::mat4 inSlot = glm::inverse(found->slotFrame) * found->transform;
    item.transform = lastPlayedTransform(*found, socket);
    item.slotFrame = item.transform * glm::inverse(inSlot);
    item.pickId = 1;
    item.section = world::Section::Featured;
    item.stance = world::ItemStance::FaceOut;
    item.inFixture = true;
    item.cluster = kLastPlayedCluster;
    world::CollectionLayout layout;
    layout.room = room.room;
    world::CollectionCluster cluster;
    cluster.id = kLastPlayedCluster;
    cluster.bounds = item.worldBounds();
    cluster.itemCount = 1;
    layout.clusters.push_back(cluster);
    layout.items.push_back(std::move(item));
    world::CollectionGroup own = *group;
    own.firstCluster = 0;
    own.clusterCount = 1;
    own.firstItem = 0;
    own.itemCount = 1;
    layout.groups.push_back(std::move(own));
    return layout;
}

std::optional<CoverItem> lastPlayedCover(std::span<const CoverItem> covers, const catalog::GameId& game) {
    const auto found =
        std::find_if(covers.begin(), covers.end(), [&](const CoverItem& cover) { return cover.game == game; });
    if (found == covers.end()) {
        return std::nullopt;
    }
    return *found;
}

} // namespace rv::app
