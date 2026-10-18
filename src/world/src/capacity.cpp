#include <rv/world/capacity.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <rv/assets/prefab_resolver.hpp>
#include <rv/geometry/socket.hpp>

namespace rv::world {
namespace {

using Json = nlohmann::json;

/// Metres to whole millimetres, once (ADR-0010 §5): the nearest millimetre, half away from zero.
std::int64_t toMillimetres(float metres) noexcept {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(metres) * 1000.0));
}

Json prefabRefJson(const PrefabRef& ref) {
    return Json{{"kind", ref.kind}, {"manifest", ref.manifest}};
}

void addUnique(std::vector<PrefabRef>& refs, const PrefabRef& ref) {
    if (!ref.manifest.empty() && std::find(refs.begin(), refs.end(), ref) == refs.end()) {
        refs.push_back(ref);
    }
}

/// The levels of a built prefab's slot sockets, each checked to number its slots 0 to count - 1.
Expected<std::vector<SlotLevelInfo>> readLevels(const geometry::ProceduralPrefab& prefab, const PrefabRef& ref) {
    std::vector<SlotLevelInfo> levels;
    for (const geometry::SlotLevel& level : geometry::slotLevels(prefab.sockets)) {
        if (level.level < 0 || level.count <= 0) {
            return makeError(ErrorCode::ValidationFailed, "a level of slots has no valid number", ref.manifest);
        }
        SlotLevelInfo info;
        info.level = static_cast<std::uint32_t>(level.level);
        info.slots = static_cast<std::uint32_t>(level.count);
        info.slotWidthMm = toMillimetres(level.slotSize.x);
        info.slotHeightMm = toMillimetres(level.slotSize.y);
        info.slotDepthMm = toMillimetres(level.slotSize.z);
        info.bottomMm = toMillimetres(level.height);
        levels.push_back(info);
    }
    // slotLevels() counts the slots of a level; the plan names them by index, so the indices must be 0 to count - 1.
    std::vector<std::uint32_t> highest(levels.size(), 0);
    for (const geometry::Socket& socket : prefab.sockets) {
        if (socket.kind != geometry::SocketKind::ShelfSlot) {
            continue;
        }
        const auto address = geometry::parseSlotId(socket.id);
        if (!address) {
            continue;
        }
        for (std::size_t i = 0; i < levels.size(); ++i) {
            if (static_cast<int>(levels[i].level) == address->level) {
                highest[i] = std::max(highest[i], static_cast<std::uint32_t>(address->index));
            }
        }
    }
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (highest[i] + 1 != levels[i].slots) {
            return makeError(ErrorCode::ValidationFailed,
                             "the slots of level " + std::to_string(levels[i].level) + " are not numbered from 0",
                             ref.manifest);
        }
    }
    return levels;
}

} // namespace

LevelFacing SlotLevelInfo::facing(const MediaFamily& family) const noexcept {
    const SlotSpec& face = family.faceOut;
    const SlotSpec& spine = family.spineOut;
    if (slotWidthMm == face.widthMm && slotHeightMm == face.heightMm && slotDepthMm == face.depthMm) {
        return LevelFacing::FaceOut;
    }
    if (slotWidthMm == spine.widthMm && slotHeightMm == spine.heightMm && slotDepthMm == spine.depthMm) {
        return LevelFacing::SpineOut;
    }
    return LevelFacing::Other;
}

std::uint64_t PrefabInfo::slots(const MediaFamily& family, LevelFacing facing) const noexcept {
    std::uint64_t total = 0;
    for (const SlotLevelInfo& level : levels) {
        if (level.facing(family) == facing) {
            total += level.slots;
        }
    }
    return total;
}

const PrefabInfo* PrefabCatalog::find(const PrefabRef& ref) const noexcept {
    const auto found = std::lower_bound(prefabs.begin(), prefabs.end(), ref,
                                        [](const PrefabInfo& info, const PrefabRef& key) { return info.ref < key; });
    return found != prefabs.end() && found->ref == ref ? &*found : nullptr;
}

void PrefabCatalog::put(PrefabInfo info) {
    const auto found = std::lower_bound(prefabs.begin(), prefabs.end(), info.ref,
                                        [](const PrefabInfo& item, const PrefabRef& key) { return item.ref < key; });
    if (found != prefabs.end() && found->ref == info.ref) {
        *found = std::move(info);
    } else {
        prefabs.insert(found, std::move(info));
    }
}

std::vector<PrefabRef> prefabsToRead(const RoomTemplate& roomTemplate, const PrefabRef& console,
                                     const MediaFamily& family) {
    std::vector<PrefabRef> refs;
    addUnique(refs, roomTemplate.heroModule);
    addUnique(refs, roomTemplate.sideModule);
    for (const TemplateSocket& socket : roomTemplate.sockets) {
        if (socket.prefab) {
            addUnique(refs, *socket.prefab);
        }
        for (const HeroPart& part : socket.parts) {
            addUnique(refs, part.prefab ? *part.prefab : console);
        }
    }
    for (const DecorationChoice& choice : roomTemplate.decoration) {
        addUnique(refs, choice.prefab);
    }
    for (const PrefabRef& prop : roomTemplate.shelfDressing) {
        addUnique(refs, prop);
    }
    addUnique(refs, family.item);
    std::sort(refs.begin(), refs.end());
    return refs;
}

Expected<PrefabInfo> readPrefabInfo(const std::filesystem::path& assetRoot, const assets::PrefabFamilies& families,
                                    const PrefabRef& ref) {
    auto resolved = assets::resolvePrefab(assetRoot, families, ref.manifest);
    if (!resolved) {
        return propagate(resolved.error());
    }
    PrefabInfo info;
    info.ref = ref;
    info.resolvedManifest = resolved->manifest ? resolved->manifest->id : std::string();
    if (resolved->source == assets::PrefabSource::Requested && !resolved->generator.empty() &&
        resolved->generator != ref.kind) {
        return makeError(ErrorCode::ValidationFailed,
                         "builds a '" + resolved->generator + "', not the '" + ref.kind + "' the data names",
                         ref.manifest);
    }
    if (resolved->prefab) {
        const geometry::ProceduralPrefab& prefab = *resolved->prefab;
        if (prefab.bounds.isEmpty()) {
            return makeError(ErrorCode::ValidationFailed, "the prefab has no bounds", ref.manifest);
        }
        info.minXMm = toMillimetres(prefab.bounds.min.x);
        info.minYMm = toMillimetres(prefab.bounds.min.y);
        info.minZMm = toMillimetres(prefab.bounds.min.z);
        info.maxXMm = toMillimetres(prefab.bounds.max.x);
        info.maxYMm = toMillimetres(prefab.bounds.max.y);
        info.maxZMm = toMillimetres(prefab.bounds.max.z);
        auto levels = readLevels(prefab, ref);
        if (!levels) {
            return propagate(levels.error());
        }
        info.levels = std::move(*levels);
        if (const geometry::Socket* light = geometry::findSocket(prefab.sockets, "LightSource")) {
            const glm::vec3 origin(light->transform[3]);
            info.lightSource = PrefabPointMm{toMillimetres(origin.x), toMillimetres(origin.y), toMillimetres(origin.z)};
        }
        return info;
    }
    // A model manifest: its nominal size, standing on the floor centred on its pivot.
    if (!resolved->manifest || !resolved->manifest->runtime.scaleMeters) {
        return makeError(ErrorCode::ValidationFailed, "a model prefab without runtime.scaleMeters", ref.manifest);
    }
    const glm::vec3 size = *resolved->manifest->runtime.scaleMeters;
    const std::int64_t width = toMillimetres(size.x);
    const std::int64_t depth = toMillimetres(size.z);
    info.minXMm = -(width / 2);
    info.maxXMm = width - width / 2;
    info.maxYMm = toMillimetres(size.y);
    info.minZMm = -(depth / 2);
    info.maxZMm = depth - depth / 2;
    return info;
}

Expected<PrefabCatalog> loadPrefabCatalog(const std::filesystem::path& assetRoot,
                                          const assets::PrefabFamilies& families, std::span<const PrefabRef> refs) {
    PrefabCatalog catalog;
    for (const PrefabRef& ref : refs) {
        auto info = readPrefabInfo(assetRoot, families, ref);
        if (!info) {
            Error error = info.error();
            error.message = "prefab " + ref.manifest + " (" + ref.kind + "): " + error.message;
            return propagate(std::move(error));
        }
        catalog.put(std::move(*info));
    }
    return catalog;
}

Json prefabCatalogToJson(const PrefabCatalog& catalog) {
    Json prefabs = Json::array();
    for (const PrefabInfo& info : catalog.prefabs) {
        Json levels = Json::array();
        for (const SlotLevelInfo& level : info.levels) {
            levels.push_back(Json{{"level", level.level},
                                  {"slots", level.slots},
                                  {"slotWidthMm", level.slotWidthMm},
                                  {"slotHeightMm", level.slotHeightMm},
                                  {"slotDepthMm", level.slotDepthMm},
                                  {"bottomMm", level.bottomMm}});
        }
        Json light = nullptr;
        if (info.lightSource) {
            light = Json{{"xMm", info.lightSource->xMm}, {"yMm", info.lightSource->yMm}, {"zMm", info.lightSource->zMm}};
        }
        prefabs.push_back(Json{{"prefab", prefabRefJson(info.ref)},
                               {"resolvedManifest", info.resolvedManifest},
                               {"minXMm", info.minXMm},
                               {"minYMm", info.minYMm},
                               {"minZMm", info.minZMm},
                               {"maxXMm", info.maxXMm},
                               {"maxYMm", info.maxYMm},
                               {"maxZMm", info.maxZMm},
                               {"levels", std::move(levels)},
                               {"lightSource", std::move(light)}});
    }
    return prefabs;
}

Expected<CollectionCapacity> collectionCapacity(const RoomTemplate& roomTemplate, const PrefabCatalog& prefabs,
                                                const MediaFamily& family) {
    const auto require = [&](const PrefabRef& ref) -> Expected<const PrefabInfo*> {
        if (const PrefabInfo* info = prefabs.find(ref)) {
            return info;
        }
        return makeError(ErrorCode::NotFound, "the prefab catalog lacks " + ref.manifest + " (" + ref.kind + ")",
                         roomTemplate.id);
    };
    CollectionCapacity capacity;
    auto hero = require(roomTemplate.heroModule);
    auto side = require(roomTemplate.sideModule);
    if (!hero) {
        return propagate(hero.error());
    }
    if (!side) {
        return propagate(side.error());
    }
    for (const std::string& socketId : roomTemplate.displayOrder) {
        const TemplateSocket* socket = roomTemplate.socket(socketId);
        if (socket == nullptr || !socket->prefab) {
            return makeError(ErrorCode::ValidationFailed, "display socket " + socketId + " has no prefab",
                             roomTemplate.id);
        }
        auto display = require(*socket->prefab);
        if (!display) {
            return propagate(display.error());
        }
        capacity.displaySlots += (*display)->slots(family, LevelFacing::FaceOut);
    }
    capacity.bandSlots = roomTemplate.heroBand.size() * (*hero)->slots(family, LevelFacing::FaceOut);
    for (const TemplateSocket& socket : roomTemplate.sockets) {
        if (socket.kind == SocketKind::HeroModule) {
            capacity.heroSpineSlots += (*hero)->slots(family, LevelFacing::SpineOut);
        }
    }
    capacity.sideModuleSlots = (*side)->slots(family, LevelFacing::SpineOut);
    for (std::uint32_t step = 0; step <= roomTemplate.maxGrowthSteps(); ++step) {
        const auto sockets = std::count_if(
            roomTemplate.sockets.begin(), roomTemplate.sockets.end(),
            [&](const TemplateSocket& socket) { return socket.kind == SocketKind::SideWall && socket.growthStep <= step; });
        capacity.sideSockets.push_back(static_cast<std::uint32_t>(sockets));
    }
    return capacity;
}

std::uint64_t reserveFor(std::uint64_t titles, std::int64_t reservePermille) noexcept {
    const auto permille = static_cast<std::uint64_t>(std::max<std::int64_t>(0, reservePermille));
    return (titles * permille + 999) / 1000;
}

std::optional<std::uint64_t> sideModulesFor(const CollectionCapacity& capacity, std::uint64_t titles,
                                            std::int64_t reservePermille) noexcept {
    const std::uint64_t faceOut = std::min(titles, capacity.displaySlots + capacity.bandSlots);
    const std::uint64_t needed = titles - faceOut + reserveFor(titles, reservePermille);
    if (needed <= capacity.heroSpineSlots) {
        return 0;
    }
    if (capacity.sideModuleSlots == 0) {
        return std::nullopt;
    }
    return (needed - capacity.heroSpineSlots + capacity.sideModuleSlots - 1) / capacity.sideModuleSlots;
}

CollectionCounts countCollection(const CollectionCapacity& capacity, std::uint64_t titles,
                                 std::int64_t reservePermille) noexcept {
    const std::uint64_t limit = capacity.maxSideSockets();
    const auto fits = [&](std::uint64_t count) {
        const auto modules = sideModulesFor(capacity, count, reservePermille);
        return modules && *modules <= limit;
    };
    CollectionCounts counts;
    counts.placed = titles;
    if (!fits(titles)) {
        // The modules a count needs never shrink as it grows, and no title needs none: the largest count that
        // fits, by bisection (0 fits, `titles` does not).
        std::uint64_t low = 0;
        std::uint64_t high = titles;
        while (high - low > 1) {
            const std::uint64_t middle = low + (high - low) / 2;
            (fits(middle) ? low : high) = middle;
        }
        counts.placed = low;
    }
    counts.overCapacity = titles - counts.placed;
    counts.featured = std::min(counts.placed, capacity.displaySlots);
    counts.band = std::min(counts.placed - counts.featured, capacity.bandSlots);
    counts.spine = counts.placed - counts.featured - counts.band;
    counts.reserve = reserveFor(counts.placed, reservePermille);
    counts.sideModules = static_cast<std::uint32_t>(sideModulesFor(capacity, counts.placed, reservePermille).value_or(0));
    for (std::size_t step = 0; step < capacity.sideSockets.size(); ++step) {
        if (capacity.sideSockets[step] >= counts.sideModules) {
            counts.growthSteps = static_cast<std::uint32_t>(step);
            break;
        }
    }
    return counts;
}

} // namespace rv::world
