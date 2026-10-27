#include <rv/app/hub_showcase.hpp>

#include <algorithm>
#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/app/room_snapshot.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/core/seed.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/world/media_family.hpp>

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

/// The cluster of the whole showcase (a whole fixture's level).
constexpr std::uint32_t kShowcaseCluster = world::kWholeFixtureLevel;

} // namespace

Expected<world::SceneData> loadShowcaseData(const std::filesystem::path& assetRoot, std::string_view family) {
    auto registry = world::loadMediaFamilies(assetRoot);
    if (!registry) {
        return propagate(registry.error());
    }
    const world::MediaFamily* found = registry->find(family);
    if (found == nullptr) {
        return makeError(ErrorCode::NotFound, "no such media family for the hub's showcase", std::string(family));
    }
    auto families = assets::loadPrefabFamilies(assetRoot);
    if (!families) {
        return propagate(families.error());
    }
    const std::vector<world::PrefabRef> refs{found->item};
    auto prefabs = world::loadScenePrefabs(assetRoot, *families, refs);
    if (!prefabs) {
        return propagate(prefabs.error());
    }
    world::SceneData data;
    data.mediaFamily = *found;
    data.prefabs = std::move(*prefabs);
    return data;
}

std::vector<ShowcaseGame> showcaseGamesOf(std::span<const catalog::RecentGame> recent,
                                          const std::map<catalog::SystemName, RoomItems>& items, std::size_t slots) {
    std::vector<ShowcaseGame> games;
    for (const catalog::RecentGame& entry : recent) {
        if (games.size() >= slots) {
            break;
        }
        ShowcaseGame game;
        game.game = entry.id;
        game.title = entry.title;
        game.system = entry.system;
        game.systemName = entry.systemFullName;
        game.favorite = entry.favorite;
        if (const auto file = items.find(entry.system); file != items.end()) {
            if (game.systemName.empty()) {
                game.systemName = file->second.systemName;
            }
            if (const RoomItemText* text = file->second.find(entry.id)) {
                game.coverPath = text->coverPath;
                game.fallbackTitle = text->fallbackTitle;
                game.year = text->year;
            }
        }
        games.push_back(std::move(game));
    }
    return games;
}

world::CollectionLayout showcaseLayout(const HubShowcase& showcase, std::span<const ShowcaseGame> games) {
    world::CollectionLayout layout;
    const world::MediaFamily& family = showcase.data.mediaFamily;
    const glm::vec3 item(static_cast<float>(family.itemWidthMm) / 1000.0f,
                         static_cast<float>(family.itemHeightMm) / 1000.0f,
                         static_cast<float>(family.itemDepthMm) / 1000.0f);
    const std::size_t count = std::min(games.size(), showcase.slots.size());
    core::Aabb bounds;
    for (std::size_t i = 0; i < count; ++i) {
        world::CollectionItem entry;
        entry.pickId = static_cast<world::PickId>(i + 1);
        entry.game = games[i].game;
        entry.section = world::Section::Featured;
        entry.stance = world::ItemStance::FaceOut;
        entry.cluster = kShowcaseCluster;
        entry.slotFrame = showcase.slots[i];
        entry.slotBox = core::Aabb::fromMinMax(glm::vec3(-0.5f * item.x, 0.0f, -0.5f * item.z),
                                               glm::vec3(0.5f * item.x, item.y, 0.5f * item.z));
        entry.transform = entry.slotFrame * geometry::slotItemTransform(geometry::SlotFacing::FaceOut, item);
        entry.title = games[i].title;
        entry.frontLayerKey = games[i].game.toString() + "/front";
        entry.spineLayerKey = games[i].game.toString() + "/spine";
        bounds = bounds.isEmpty() ? entry.worldBounds() : bounds.merged(entry.worldBounds());
        layout.items.push_back(std::move(entry));
    }
    if (!layout.items.empty()) {
        world::CollectionCluster cluster;
        cluster.id = kShowcaseCluster;
        cluster.bounds = bounds;
        cluster.itemCount = static_cast<std::uint32_t>(layout.items.size());
        layout.clusters.push_back(cluster);
    }
    world::CollectionGroup group;
    group.mediaFamily = family.id;
    group.item = family.item;
    group.itemSize = item;
    group.shellMaterial = family.shellMaterial;
    group.labelMaterial = family.labelMaterial;
    group.clusterCount = static_cast<std::uint32_t>(layout.clusters.size());
    group.itemCount = static_cast<std::uint32_t>(layout.items.size());
    layout.groups.push_back(std::move(group));
    return layout;
}

std::vector<CoverItem> showcaseCovers(const HubShowcase& showcase, const world::CollectionLayout& layout,
                                      std::span<const ShowcaseGame> games) {
    std::vector<CoverItem> covers;
    const world::MediaFamily& family = showcase.data.mediaFamily;
    for (std::size_t i = 0; i < layout.items.size() && i < games.size(); ++i) {
        const world::CollectionItem& item = layout.items[i];
        const ShowcaseGame& game = games[i];
        CoverItem cover;
        cover.game = game.game;
        cover.title = game.title;
        cover.fallbackTitle = game.fallbackTitle;
        cover.systemName = game.systemName;
        cover.year = game.year;
        cover.seed = core::deriveSeed("hub-showcase", game.game.toString());
        cover.coverPath = game.coverPath;
        cover.frontKey = item.frontLayerKey;
        cover.spineKey = item.spineLayerKey;
        cover.label = family.coverAspect();
        cover.spine = family.spineAspect();
        covers.push_back(std::move(cover));
    }
    return covers;
}

ShowcaseContent buildShowcaseContent(const std::filesystem::path& cacheDir, const catalog::LibraryId& library,
                                     WorldCatalogReader& reader, const HubShowcase& showcase) {
    const Clock::time_point start = Clock::now();
    ShowcaseContent content;
    std::vector<catalog::RecentGame> recent;
    {
        const std::lock_guard lock(reader.mutex);
        if (!reader.query) {
            content.error = Error{ErrorCode::Unavailable, "the catalog's world reader is closed", "showcase"};
        } else {
            catalog::RecentGamesQuery query;
            query.limit = showcase.slots.size();
            if (auto machine = catalog::machineLocalTimeToUtc()) {
                query.localToUtc = std::move(*machine);
            } else {
                query.localToUtc = catalog::fixedOffsetLocalTimeToUtc(std::chrono::minutes(0));
            }
            if (auto games = reader.query->favoritesAndRecent(query)) {
                recent = std::move(*games);
            } else {
                content.error = games.error();
            }
        }
    }
    if (content.error) {
        spdlog::warn("World: the hub's showcase cannot read the favourite and recent games: {}",
                     content.error->toString());
    }
    // Each system's items file once (the covers' paths of the games its room places).
    std::map<catalog::SystemName, RoomItems> items;
    for (const catalog::RecentGame& game : recent) {
        if (items.contains(game.system)) {
            continue;
        }
        if (auto file = readRoomItems(roomItemsFile(cacheDir, library, game.system))) {
            items.emplace(game.system, std::move(*file));
        } else {
            items.emplace(game.system, RoomItems{});
        }
    }
    content.games = showcaseGamesOf(recent, items, showcase.slots.size());
    content.layout = showcaseLayout(showcase, content.games);
    content.covers = showcaseCovers(showcase, content.layout, content.games);
    content.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return content;
}

} // namespace rv::app
