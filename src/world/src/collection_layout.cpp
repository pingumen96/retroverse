#include <rv/world/collection_layout.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/geometry/socket.hpp>

#include "room_parts.hpp"

namespace rv::world {
namespace {

using nlohmann::json;

constexpr std::uint32_t kMaxPickSockets = 254;
constexpr std::uint32_t kMaxPickLevel = 1023;
constexpr std::uint32_t kMaxPickIndex = 16383;
/// Pieces thinner than this (meters) are no occluder.
constexpr float kMinOccluderSide = 1e-4f;

double meters(std::int64_t millimetres) noexcept {
    return static_cast<double>(millimetres) / 1000.0;
}

std::int64_t toMm(float value) noexcept {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(value) * 1000.0));
}

/// A value in millionths (micrometres for lengths), for the canonical document.
std::int64_t micro(float value) noexcept {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(value) * 1e6));
}

json matrixJson(const glm::mat4& m) {
    json values = json::array();
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            values.push_back(micro(m[column][row]));
        }
    }
    return values;
}

json boxJson(const core::Aabb& box) {
    return json::array(
        {micro(box.min.x), micro(box.min.y), micro(box.min.z), micro(box.max.x), micro(box.max.y), micro(box.max.z)});
}

const Room* findRoom(const WorldPlan& plan, const RoomId& id) noexcept {
    for (const Room& room : plan.rooms) {
        if (room.id == id) {
            return &room;
        }
    }
    return nullptr;
}

/// The index of each of the template's sockets.
std::map<std::string, std::uint32_t, std::less<>> socketIndices(const RoomTemplate& roomTemplate) {
    std::map<std::string, std::uint32_t, std::less<>> indices;
    for (std::size_t i = 0; i < roomTemplate.sockets.size(); ++i) {
        indices.emplace(roomTemplate.sockets[i].id, static_cast<std::uint32_t>(i));
    }
    return indices;
}

/// The slot volume a socket holds, in whole millimetres.
struct SizeMm {
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::int64_t depth = 0;
};

SizeMm socketSizeMm(const geometry::Socket& socket) noexcept {
    const glm::vec3 size = geometry::socketSize(socket);
    return SizeMm{toMm(size.x), toMm(size.y), toMm(size.z)};
}

bool holds(const SizeMm& size, const SlotSpec& spec) noexcept {
    return size.width == spec.widthMm && size.height == spec.heightMm && size.depth == spec.depthMm;
}

/// A box in a frame, as the room sees it.
core::Aabb boxIn(const glm::mat4& frame, const core::Aabb& box) noexcept {
    return box.transformed(frame);
}

void addOccluder(std::vector<core::Aabb>& occluders, const core::Aabb& box) {
    if (box.isEmpty()) {
        return;
    }
    const glm::vec3 size = box.size();
    if (size.x < kMinOccluderSide || size.y < kMinOccluderSide || size.z < kMinOccluderSide) {
        return;
    }
    occluders.push_back(box);
}

/// A box from its corners in a prefab's frame, added to the room's occluders through the fixture's transform.
void addPiece(std::vector<core::Aabb>& occluders, const glm::mat4& fixture, glm::vec3 low, glm::vec3 high) {
    if (high.x - low.x < kMinOccluderSide || high.y - low.y < kMinOccluderSide || high.z - low.z < kMinOccluderSide) {
        return;
    }
    addOccluder(occluders, boxIn(fixture, core::Aabb::fromMinMax(low, high)));
}

/// The free volumes of a prefab's levels ("Level_<n>" anchors), in the prefab's frame, bottom to top.
std::vector<core::Aabb> levelVolumes(const geometry::ProceduralPrefab& prefab) {
    std::vector<core::Aabb> volumes;
    for (const geometry::Socket& socket : prefab.sockets) {
        if (geometry::parseLevelId(socket.id) && !socket.bounds.isEmpty()) {
            volumes.push_back(socket.bounds.transformed(socket.transform));
        }
    }
    std::sort(volumes.begin(), volumes.end(),
              [](const core::Aabb& a, const core::Aabb& b) { return a.min.y < b.min.y; });
    return volumes;
}

/// A shelf's or the wall shelf's solid pieces: its bounds outside its levels' free volumes, each opened to the front.
void stackedOccluders(std::vector<core::Aabb>& occluders, const geometry::ProceduralPrefab& prefab,
                      const glm::mat4& fixture) {
    const std::vector<core::Aabb> levels = levelVolumes(prefab);
    if (levels.empty()) {
        return;
    }
    const core::Aabb& b = prefab.bounds;
    float left = levels.front().min.x;
    float right = levels.front().max.x;
    float back = levels.front().min.z;
    for (const core::Aabb& level : levels) {
        left = std::min(left, level.min.x);
        right = std::max(right, level.max.x);
        back = std::min(back, level.min.z);
    }
    addPiece(occluders, fixture, b.min, glm::vec3(left, b.max.y, b.max.z));
    addPiece(occluders, fixture, glm::vec3(right, b.min.y, b.min.z), b.max);
    addPiece(occluders, fixture, glm::vec3(left, b.min.y, b.min.z), glm::vec3(right, b.max.y, back));
    float floor = b.min.y;
    for (const core::Aabb& level : levels) {
        addPiece(occluders, fixture, glm::vec3(left, floor, back), glm::vec3(right, level.min.y, b.max.z));
        floor = std::max(floor, level.max.y);
    }
    addPiece(occluders, fixture, glm::vec3(left, floor, back), glm::vec3(right, b.max.y, b.max.z));
}

/// A display case's solid pieces: its base under the levels and its back; the front, the sides and the top are glass
/// (the corner posts are too slim to hide an item).
void vitrineOccluders(std::vector<core::Aabb>& occluders, const geometry::ProceduralPrefab& prefab,
                      const glm::mat4& fixture) {
    const std::vector<core::Aabb> levels = levelVolumes(prefab);
    if (levels.empty()) {
        return;
    }
    core::Aabb open;
    for (const core::Aabb& level : levels) {
        open.expand(level);
    }
    const core::Aabb& b = prefab.bounds;
    addPiece(occluders, fixture, b.min, glm::vec3(b.max.x, open.min.y, b.max.z));
    addPiece(occluders, fixture, glm::vec3(open.min.x, open.min.y, b.min.z), glm::vec3(open.max.x, b.max.y, open.min.z));
}

bool hasSlots(const geometry::ProceduralPrefab& prefab) noexcept {
    return std::any_of(prefab.sockets.begin(), prefab.sockets.end(), [](const geometry::Socket& socket) {
        return socket.kind == geometry::SocketKind::ShelfSlot && geometry::parseSlotId(socket.id).has_value();
    });
}

void fixtureOccluders(std::vector<core::Aabb>& occluders, const Fixture& fixture, const ScenePrefab& prefab) {
    const glm::mat4 frame = room_frames::fixtureTransform(fixture);
    if (!prefab.prefab) {
        // A model: its box as the plan measured it, standing on the floor at its pivot.
        const glm::vec3 half(static_cast<float>(meters(fixture.widthMm)) * 0.5f, 0.0f,
                             static_cast<float>(meters(fixture.depthMm)) * 0.5f);
        addPiece(occluders, frame, glm::vec3(-half.x, 0.0f, -half.z),
                 glm::vec3(half.x, static_cast<float>(meters(fixture.heightMm)), half.z));
        return;
    }
    const geometry::ProceduralPrefab& built = *prefab.prefab;
    if (prefab.generator == "shelf" || prefab.generator == "wall_shelf") {
        stackedOccluders(occluders, built, frame);
    } else if (prefab.generator == "display_case") {
        vitrineOccluders(occluders, built, frame);
    } else if (prefab.generator == "rug" || hasSlots(built)) {
        // The rug lies on the floor; a fixture of another kind with slots has no rule for its open volumes: nothing.
    } else {
        addOccluder(occluders, boxIn(frame, built.bounds));
    }
}

std::uint32_t clusterId(std::uint32_t socketIndex, std::uint32_t level) noexcept {
    return ((socketIndex + 1u) << 10) | level;
}

/// The slot sockets of the prefabs a layout reads, indexed by (level, index) once per prefab.
class SlotSockets {
public:
    [[nodiscard]] const geometry::Socket* find(const geometry::ProceduralPrefab& prefab, std::uint32_t level,
                                               std::uint32_t index) {
        auto [it, inserted] = m_byPrefab.try_emplace(&prefab);
        if (inserted) {
            for (const geometry::Socket& socket : prefab.sockets) {
                if (socket.kind != geometry::SocketKind::ShelfSlot) {
                    continue;
                }
                if (const auto address = geometry::parseSlotId(socket.id)) {
                    it->second.emplace(std::make_pair(static_cast<std::uint32_t>(address->level),
                                                      static_cast<std::uint32_t>(address->index)),
                                       &socket);
                }
            }
        }
        const auto found = it->second.find({level, index});
        return found != it->second.end() ? found->second : nullptr;
    }

private:
    std::map<const geometry::ProceduralPrefab*, std::map<std::pair<std::uint32_t, std::uint32_t>, const geometry::Socket*>>
        m_byPrefab;
};

} // namespace

std::vector<CollectionTitle> collectionTitles(const catalog::RoomSnapshot& snapshot) {
    std::vector<CollectionTitle> titles;
    titles.reserve(snapshot.entries.size());
    for (const catalog::RoomEntry& entry : snapshot.entries) {
        titles.push_back(
            CollectionTitle{entry.id, entry.title, entry.availability == catalog::Availability::Available});
    }
    std::sort(titles.begin(), titles.end(),
              [](const CollectionTitle& a, const CollectionTitle& b) { return a.game < b.game; });
    return titles;
}

Expected<PickId> slotPickId(const RoomTemplate& roomTemplate, const SlotId& slot) {
    if (roomTemplate.sockets.size() > kMaxPickSockets) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("{} sockets, more than the {} pick ids can tell apart",
                                     roomTemplate.sockets.size(), kMaxPickSockets),
                         roomTemplate.id);
    }
    if (slot.level() > kMaxPickLevel || slot.index() > kMaxPickIndex) {
        return makeError(ErrorCode::OutOfRange, "a level or index beyond what a pick id holds", slot.toString());
    }
    for (std::size_t i = 0; i < roomTemplate.sockets.size(); ++i) {
        if (roomTemplate.sockets[i].id == slot.fixture().socket()) {
            return static_cast<PickId>(((static_cast<std::uint32_t>(i) + 1u) << 24) | (slot.level() << 14) |
                                       slot.index());
        }
    }
    return makeError(ErrorCode::NotFound, "the template has no such socket", slot.toString());
}

scene::InstanceAttributes CollectionItem::attributes() const noexcept {
    return scene::InstanceAttributes{0, 0, flags, pickId};
}

scene::InstanceData CollectionItem::instance() const noexcept {
    return scene::InstanceData{transform, attributes()};
}

PickTarget CollectionItem::pickTarget() const noexcept {
    return PickTarget{pickId, cluster, slotFrame, faceTargetBox(slotBox)};
}

core::Aabb CollectionItem::worldBounds() const noexcept {
    return slotBox.transformed(slotFrame);
}

const CollectionItem* CollectionLayout::find(PickId id) const noexcept {
    for (const CollectionItem& item : items) {
        if (item.pickId == id) {
            return &item;
        }
    }
    return nullptr;
}

const CollectionItem* CollectionLayout::findGame(const catalog::GameId& game) const noexcept {
    for (const CollectionItem& item : items) {
        if (item.game == game) {
            return &item;
        }
    }
    return nullptr;
}

std::vector<PickTarget> CollectionLayout::pickTargets() const {
    std::vector<PickTarget> targets;
    targets.reserve(items.size());
    for (const CollectionItem& item : items) {
        targets.push_back(item.pickTarget());
    }
    return targets;
}

Expected<CollectionLayout> collectionLayout(const WorldPlan& plan, const RoomId& roomId, const SceneData& data,
                                            std::span<const CollectionTitle> titles) {
    const Room* room = findRoom(plan, roomId);
    if (room == nullptr) {
        return makeError(ErrorCode::NotFound, "the plan has no such room", roomId.toString());
    }
    const MediaFamily& family = data.mediaFamily;
    const glm::vec3 itemSize(static_cast<float>(meters(family.itemWidthMm)),
                             static_cast<float>(meters(family.itemHeightMm)),
                             static_cast<float>(meters(family.itemDepthMm)));

    std::vector<CollectionTitle> sortedTitles(titles.begin(), titles.end());
    std::sort(sortedTitles.begin(), sortedTitles.end(),
              [](const CollectionTitle& a, const CollectionTitle& b) { return a.game < b.game; });
    const auto titleOf = [&](const catalog::GameId& game) -> const CollectionTitle* {
        const auto it = std::lower_bound(sortedTitles.begin(), sortedTitles.end(), game,
                                         [](const CollectionTitle& t, const catalog::GameId& g) { return t.game < g; });
        return it != sortedTitles.end() && it->game == game ? &*it : nullptr;
    };

    std::map<FixtureId, const Fixture*> fixtures;
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() == roomId) {
            fixtures.emplace(fixture.id, &fixture);
        }
    }
    const auto sockets = socketIndices(data.roomTemplate);
    const auto socketIndexOf = [&](const FixtureId& id) -> std::optional<std::uint32_t> {
        const auto it = sockets.find(id.socket());
        return it != sockets.end() ? std::optional<std::uint32_t>(it->second) : std::nullopt;
    };

    std::vector<const Placement*> placements;
    for (const Placement& placement : plan.placements) {
        if (placement.slot.fixture().room() == roomId) {
            placements.push_back(&placement);
        }
    }
    std::sort(placements.begin(), placements.end(), [](const Placement* a, const Placement* b) { return *a < *b; });

    CollectionLayout layout;
    layout.room = roomId;
    SlotSockets slotSockets;
    for (std::size_t p = 0; p < placements.size(); ++p) {
        const Placement& placement = *placements[p];
        if (p > 0 && placements[p - 1]->slot == placement.slot) {
            return makeError(ErrorCode::ValidationFailed, "two placements in one slot", placement.slot.toString());
        }
        const auto skip = [&](std::string reason) {
            layout.skipped.push_back(SkippedPlacement{placement, std::move(reason)});
        };
        const auto fixtureIt = fixtures.find(placement.slot.fixture());
        if (fixtureIt == fixtures.end()) {
            return makeError(ErrorCode::NotFound, "the plan has no fixture of the placement's slot",
                             placement.slot.toString());
        }
        const Fixture& fixture = *fixtureIt->second;
        const ScenePrefab* prefab = data.prefabs.find(fixture.prefab);
        if (prefab == nullptr) {
            return makeError(ErrorCode::NotFound, "no prefab for the fixture's " + fixture.prefab.manifest,
                             fixture.id.toString());
        }
        if (!prefab->prefab) {
            skip("the fixture is a model without slot sockets");
            continue;
        }
        const auto level = std::find_if(fixture.levels.begin(), fixture.levels.end(), [&](const FixtureLevel& l) {
            return l.level == placement.slot.level();
        });
        if (level == fixture.levels.end() || placement.slot.index() >= level->slots) {
            skip("the plan's fixture has no such slot");
            continue;
        }
        const geometry::Socket* socket = slotSockets.find(*prefab->prefab, placement.slot.level(), placement.slot.index());
        if (socket == nullptr) {
            skip("the fixture's prefab has no such slot");
            continue;
        }
        const SizeMm size = socketSizeMm(*socket);
        ItemStance stance = ItemStance::FaceOut;
        if (holds(size, family.faceOut)) {
            stance = ItemStance::FaceOut;
        } else if (holds(size, family.spineOut)) {
            stance = ItemStance::SpineOut;
        } else {
            skip(fmt::format("the slot holds a {} x {} x {} mm item, not the family's", size.width, size.height,
                             size.depth));
            continue;
        }
        if (isFaceOut(level->section) != (stance == ItemStance::FaceOut)) {
            skip(fmt::format("a {} slot in the {} section", toString(stance), toString(level->section)));
            continue;
        }
        const auto socketIndex = socketIndexOf(fixture.id);
        if (!socketIndex) {
            return makeError(ErrorCode::NotFound, "the template has no such socket", fixture.id.toString());
        }
        auto pickId = slotPickId(data.roomTemplate, placement.slot);
        if (!pickId) {
            return propagate(pickId.error());
        }

        CollectionItem item;
        item.pickId = *pickId;
        item.game = placement.game;
        item.slot = placement.slot;
        item.section = level->section;
        item.stance = stance;
        item.cluster =
            clusterId(*socketIndex, prefab->generator == "shelf" ? placement.slot.level() : kWholeFixtureLevel);
        // The offset (ADR-0010 §6): pushed in along the slot's -Z, turned about its +Y through the item's centre.
        const glm::mat4 offset = glm::translate(glm::mat4(1.0f),
                                                glm::vec3(0.0f, 0.0f, -static_cast<float>(meters(placement.insetMm)))) *
                                 room_frames::yawRotation(placement.yawCdeg);
        item.slotFrame = room_frames::fixtureTransform(fixture) * socket->transform * offset;
        item.slotBox = socket->bounds;
        item.transform =
            item.slotFrame * geometry::slotItemTransform(stance == ItemStance::FaceOut ? geometry::SlotFacing::FaceOut
                                                                                         : geometry::SlotFacing::SpineOut,
                                                         itemSize);
        const CollectionTitle* title = titleOf(placement.game);
        if (title == nullptr || !title->available) {
            item.flags |= scene::instance_flags::kUnavailable;
        }
        item.title = title != nullptr ? title->title : std::string();
        const std::string gameText = placement.game.toString();
        item.frontLayerKey = gameText + "/front";
        item.spineLayerKey = gameText + "/spine";
        layout.items.push_back(std::move(item));
    }
    std::stable_sort(layout.items.begin(), layout.items.end(), [](const CollectionItem& a, const CollectionItem& b) {
        return a.cluster != b.cluster ? a.cluster < b.cluster : a.slot < b.slot;
    });

    for (std::size_t i = 0; i < layout.items.size(); ++i) {
        const CollectionItem& item = layout.items[i];
        if (layout.clusters.empty() || layout.clusters.back().id != item.cluster) {
            CollectionCluster cluster;
            cluster.id = item.cluster;
            cluster.fixture = item.slot.fixture();
            cluster.level = item.cluster & kMaxPickLevel;
            cluster.firstItem = static_cast<std::uint32_t>(i);
            layout.clusters.push_back(cluster);
        }
        CollectionCluster& cluster = layout.clusters.back();
        cluster.bounds.expand(item.worldBounds());
        ++cluster.itemCount;
    }
    if (!layout.items.empty()) {
        CollectionGroup group;
        group.mediaFamily = family.id;
        group.item = family.item;
        group.itemSize = itemSize;
        group.clusterCount = static_cast<std::uint32_t>(layout.clusters.size());
        group.itemCount = static_cast<std::uint32_t>(layout.items.size());
        layout.groups.push_back(std::move(group));
    }

    // Occluders: the walls, the fixtures, the dressing.
    const room_frames::ShellPlacement shell = room_frames::roomShell(*room, data.roomTemplate);
    auto shellPrefab = geometry::buildPrefab("room_shell", shell.params);
    if (!shellPrefab) {
        return propagate(shellPrefab.error());
    }
    const glm::mat4 shellFrame = glm::translate(glm::mat4(1.0f), shell.position);
    for (const geometry::PrefabCollider& collider : shellPrefab->colliders) {
        addOccluder(layout.occluders, boxIn(shellFrame, collider.box));
    }
    for (const auto& [id, fixture] : fixtures) {
        const ScenePrefab* prefab = data.prefabs.find(fixture->prefab);
        if (prefab == nullptr) {
            return makeError(ErrorCode::NotFound, "no prefab for the fixture's " + fixture->prefab.manifest,
                             id.toString());
        }
        fixtureOccluders(layout.occluders, *fixture, *prefab);
    }
    for (const DressedLevel& dressed : plan.dressing) {
        if (dressed.level.fixture().room() != roomId) {
            continue;
        }
        const auto fixtureIt = fixtures.find(dressed.level.fixture());
        if (fixtureIt == fixtures.end()) {
            return makeError(ErrorCode::NotFound, "the plan has no fixture of the dressed level",
                             dressed.level.toString());
        }
        const ScenePrefab* fixturePrefab = data.prefabs.find(fixtureIt->second->prefab);
        const ScenePrefab* prop = data.prefabs.find(dressed.prop);
        if (fixturePrefab == nullptr || prop == nullptr) {
            return makeError(ErrorCode::NotFound, "no prefab for the dressed level or its prop",
                             dressed.level.toString());
        }
        std::string reason;
        auto placed = detail::dressingProp(*room, *fixtureIt->second, *fixturePrefab, dressed, *prop, reason);
        if (!placed) {
            return propagate(placed.error());
        }
        if (*placed) {
            const glm::vec3 half = 0.5f * (*placed)->size;
            addOccluder(layout.occluders,
                        boxIn((*placed)->transform, core::Aabb::fromMinMax(glm::vec3(-half.x, 0.0f, -half.z),
                                                                           glm::vec3(half.x, (*placed)->size.y, half.z))));
        }
    }

    // Navigation: the fixtures with slots, by wall (left to right as one faces it), the free-standing ones alone.
    std::map<SlotId, PickId> itemAt;
    for (const CollectionItem& item : layout.items) {
        itemAt.emplace(item.slot, item.pickId);
    }
    struct NavCandidate {
        float along = 0.0f;
        FixtureId id;
        NavFixture fixture;
    };
    std::array<std::vector<NavCandidate>, 4> walls;
    std::vector<NavCandidate> freeStanding;
    for (const auto& [id, fixture] : fixtures) {
        const ScenePrefab* prefab = data.prefabs.find(fixture->prefab);
        if (prefab == nullptr || !prefab->prefab || fixture->levels.empty()) {
            continue;
        }
        const glm::mat4 frame = room_frames::fixtureTransform(*fixture);
        const glm::vec3 right = glm::vec3(frame * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        std::vector<NavLevel> levels;
        for (const FixtureLevel& level : fixture->levels) {
            NavLevel nav;
            bool first = true;
            for (std::uint32_t index = 0; index < level.slots; ++index) {
                const geometry::Socket* socket = slotSockets.find(*prefab->prefab, level.level, index);
                if (socket == nullptr) {
                    continue;
                }
                const glm::vec3 centre = glm::vec3(frame * socket->transform * glm::vec4(socket->bounds.center(), 1.0f));
                if (first) {
                    nav.height = centre.y;
                    first = false;
                }
                const SlotId slot = *SlotId::make(id, level.level, index);
                const auto item = itemAt.find(slot);
                nav.slots.push_back(NavSlot{glm::dot(centre, right), item != itemAt.end() ? item->second : kNoPick});
            }
            if (!nav.slots.empty()) {
                levels.push_back(std::move(nav));
            }
        }
        if (levels.empty()) {
            continue;
        }
        std::sort(levels.begin(), levels.end(), [](const NavLevel& a, const NavLevel& b) { return a.height < b.height; });
        NavCandidate candidate;
        candidate.along = glm::dot(glm::vec3(frame[3]), right);
        candidate.id = id;
        candidate.fixture.levels = std::move(levels);
        Wall wall = Wall::None;
        for (const TemplateSocket& socket : data.roomTemplate.sockets) {
            if (socket.id == id.socket()) {
                wall = socket.wall;
                break;
            }
        }
        if (wall == Wall::None) {
            freeStanding.push_back(std::move(candidate));
        } else {
            walls[static_cast<std::size_t>(wall)].push_back(std::move(candidate));
        }
    }
    for (std::vector<NavCandidate>& wall : walls) {
        if (wall.empty()) {
            continue;
        }
        std::stable_sort(wall.begin(), wall.end(),
                         [](const NavCandidate& a, const NavCandidate& b) { return a.along < b.along; });
        NavRun run;
        for (NavCandidate& candidate : wall) {
            run.fixtures.push_back(std::move(candidate.fixture));
        }
        layout.navigation.push_back(std::move(run));
    }
    for (NavCandidate& candidate : freeStanding) {
        NavRun run;
        run.fixtures.push_back(std::move(candidate.fixture));
        layout.navigation.push_back(std::move(run));
    }
    return layout;
}

json collectionLayoutToJson(const CollectionLayout& layout) {
    json items = json::array();
    for (const CollectionItem& item : layout.items) {
        items.push_back(json{{"pickId", item.pickId},
                             {"game", item.game.toString()},
                             {"slot", item.slot.toString()},
                             {"section", std::string(toString(item.section))},
                             {"stance", std::string(toString(item.stance))},
                             {"cluster", item.cluster},
                             {"flags", item.flags},
                             {"title", item.title},
                             {"frontLayerKey", item.frontLayerKey},
                             {"spineLayerKey", item.spineLayerKey},
                             {"slotFrame", matrixJson(item.slotFrame)},
                             {"slotBox", boxJson(item.slotBox)},
                             {"transform", matrixJson(item.transform)}});
    }
    json clusters = json::array();
    for (const CollectionCluster& cluster : layout.clusters) {
        clusters.push_back(json{{"id", cluster.id},
                                {"fixture", cluster.fixture.toString()},
                                {"level", cluster.level},
                                {"bounds", boxJson(cluster.bounds)},
                                {"firstItem", cluster.firstItem},
                                {"itemCount", cluster.itemCount}});
    }
    json groups = json::array();
    for (const CollectionGroup& group : layout.groups) {
        groups.push_back(json{{"mediaFamily", group.mediaFamily},
                              {"item", json{{"manifest", group.item.manifest}, {"kind", group.item.kind}}},
                              {"itemSize", json::array({micro(group.itemSize.x), micro(group.itemSize.y),
                                                        micro(group.itemSize.z)})},
                              {"firstCluster", group.firstCluster},
                              {"clusterCount", group.clusterCount},
                              {"firstItem", group.firstItem},
                              {"itemCount", group.itemCount}});
    }
    json occluders = json::array();
    for (const core::Aabb& box : layout.occluders) {
        occluders.push_back(boxJson(box));
    }
    json navigation = json::array();
    for (const NavRun& run : layout.navigation) {
        json fixturesJson = json::array();
        for (const NavFixture& fixture : run.fixtures) {
            json levels = json::array();
            for (const NavLevel& level : fixture.levels) {
                json slots = json::array();
                for (const NavSlot& slot : level.slots) {
                    slots.push_back(json::array({micro(slot.along), slot.item}));
                }
                levels.push_back(json{{"height", micro(level.height)}, {"slots", std::move(slots)}});
            }
            fixturesJson.push_back(json{{"levels", std::move(levels)}});
        }
        navigation.push_back(json{{"fixtures", std::move(fixturesJson)}});
    }
    json skipped = json::array();
    for (const SkippedPlacement& entry : layout.skipped) {
        skipped.push_back(json{{"slot", entry.placement.slot.toString()},
                               {"game", entry.placement.game.toString()},
                               {"reason", entry.reason}});
    }
    return json{{"room", layout.room.toString()}, {"items", std::move(items)},    {"clusters", std::move(clusters)},
                {"groups", std::move(groups)},    {"occluders", std::move(occluders)}, {"navigation", std::move(navigation)},
                {"skipped", std::move(skipped)}};
}

} // namespace rv::world
