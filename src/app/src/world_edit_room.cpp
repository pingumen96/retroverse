#include <rv/app/world_edit_room.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace rv::app {
namespace {

/// A fixture's box (millimetres in its room's frame) in the scene's frame (meters).
[[nodiscard]] core::Aabb sceneBoxOf(const world::FixtureBox& box, const glm::mat4& toScene) noexcept {
    const auto meters = [](std::int64_t millimetres) {
        return static_cast<float>(static_cast<double>(millimetres) / 1000.0);
    };
    return core::Aabb::fromMinMax(glm::vec3(meters(box.minXMm), meters(box.minYMm), meters(box.minZMm)),
                                  glm::vec3(meters(box.maxXMm), meters(box.maxYMm), meters(box.maxZMm)))
        .transformed(toScene);
}

[[nodiscard]] std::int64_t millimetres(float meters) noexcept {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(meters) * 1000.0));
}

/// The pose of a fixture standing at `place` in its room's frame: its pivot on the floor, turned about +Y.
[[nodiscard]] glm::mat4 poseOf(const world::FixtureTransform& place) noexcept {
    const glm::mat4 at = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(place.xMm) / 1000.0f, 0.0f,
                                                                   static_cast<float>(place.zMm) / 1000.0f));
    return glm::rotate(at, glm::radians(static_cast<float>(place.yawCdeg) / 100.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

} // namespace

std::vector<world::FixtureBox> fixedFixtureBoxes(const world::WorldPlan& plan, const world::RoomId& room) {
    std::vector<world::FixtureBox> boxes;
    for (const world::Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() == room &&
            (fixture.role == world::FixtureRole::HeroCorner || fixture.role == world::FixtureRole::HeroModule)) {
            boxes.push_back(world::fixtureBox(fixture));
        }
    }
    return boxes;
}

glm::mat4 fixtureMove(const world::FixtureTransform& from, const world::FixtureTransform& place,
                      const glm::mat4& toScene) noexcept {
    return toScene * poseOf(place) * glm::inverse(poseOf(from)) * glm::inverse(toScene);
}

Expected<std::unique_ptr<EditRoom>> EditRoom::build(std::shared_ptr<const world::CollectionLayout> layout,
                                                    std::vector<world::EditableFixture> fixtures,
                                                    const EditRoomFrame& frame, std::span<const world::FixtureBox> fixed) {
    std::unique_ptr<EditRoom> room(new EditRoom());
    room->m_layout = std::move(layout);
    room->m_fixtures = std::move(fixtures);
    room->m_frame = frame;
    room->m_fixtureBoxes.reserve(room->m_fixtures.size());
    for (const world::EditableFixture& fixture : room->m_fixtures) {
        room->m_fixtureBoxes.push_back(sceneBoxOf(fixture.box, frame.toScene));
    }
    room->m_fixedBoxes.reserve(fixed.size());
    for (const world::FixtureBox& box : fixed) {
        room->m_fixedBoxes.push_back(sceneBoxOf(box, frame.toScene));
    }
    const world::CollectionLayout& shown = *room->m_layout;
    std::vector<world::PickTarget> targets = shown.pickTargets();
    for (std::size_t index = 0; index < shown.items.size(); ++index) {
        room->m_items.emplace(shown.items[index].pickId, index);
        room->m_byGame.emplace(shown.items[index].game, shown.items[index].pickId);
    }
    for (std::size_t index = 0; index < shown.freeSlots.size(); ++index) {
        const world::FreeSlot& free = shown.freeSlots[index];
        if (room->m_items.contains(free.pickId)) {
            continue; // never both (the layout lists a slot once); kept safe all the same
        }
        room->m_free.emplace(free.pickId, index);
        targets.push_back(free.pickTarget());
    }
    auto index = world::PickIndex::build(targets, shown.occluders);
    if (!index) {
        return propagate(index.error());
    }
    room->m_index = std::move(*index);
    // A held game's ray passes the dressing props (the last occluders): their dressing gives way where it lands.
    const std::size_t solid = shown.occluders.size() - std::min(shown.dressingOccluders, shown.occluders.size());
    auto held = world::PickIndex::build(targets, std::span<const core::Aabb>(shown.occluders.data(), solid));
    if (!held) {
        return propagate(held.error());
    }
    room->m_heldIndex = std::move(*held);
    // Every slot that holds an item or is free is a step's stop (the layout's runs name each slot's pick id).
    std::vector<world::NavRun> runs = shown.navigation;
    for (world::NavRun& run : runs) {
        for (world::NavFixture& fixture : run.fixtures) {
            for (world::NavLevel& level : fixture.levels) {
                for (world::NavSlot& slot : level.slots) {
                    const bool stop = slot.slot != world::kNoPick &&
                                      (room->m_items.contains(slot.slot) || room->m_free.contains(slot.slot));
                    slot.item = stop ? slot.slot : world::kNoPick;
                }
            }
        }
    }
    auto navigation = world::SlotNavigation::build(std::move(runs));
    if (!navigation) {
        return propagate(navigation.error());
    }
    room->m_navigation = std::move(*navigation);
    return room;
}

const world::CollectionItem* EditRoom::item(world::PickId pick) const noexcept {
    const auto found = m_items.find(pick);
    return found != m_items.end() ? &m_layout->items[found->second] : nullptr;
}

const world::FreeSlot* EditRoom::freeSlot(world::PickId pick) const noexcept {
    const auto found = m_free.find(pick);
    return found != m_free.end() ? &m_layout->freeSlots[found->second] : nullptr;
}

std::optional<EditTarget> EditRoom::target(world::PickId pick) const {
    if (const world::CollectionItem* placed = item(pick)) {
        return EditTarget{pick, placed->slot, placed->game, placed->stance == world::ItemStance::FaceOut, false};
    }
    if (const world::FreeSlot* free = freeSlot(pick)) {
        return EditTarget{pick, free->slot, std::nullopt, free->stance == world::ItemStance::FaceOut, free->dressed};
    }
    return std::nullopt;
}

world::PickId EditRoom::pickOf(const catalog::GameId& game) const noexcept {
    const auto found = m_byGame.find(game);
    return found != m_byGame.end() ? found->second : world::kNoPick;
}

std::optional<world::PickId> EditRoom::step(world::PickId from, world::StepDirection direction) const noexcept {
    return m_navigation.step(from, direction);
}

std::optional<world::CollectionItem> EditRoom::asItem(world::PickId pick) const {
    if (const world::CollectionItem* placed = item(pick)) {
        return *placed;
    }
    const world::FreeSlot* free = freeSlot(pick);
    if (free == nullptr) {
        return std::nullopt;
    }
    world::CollectionItem standIn;
    standIn.pickId = free->pickId;
    standIn.slot = free->slot;
    standIn.section = free->section;
    standIn.stance = free->stance;
    standIn.cluster = free->cluster;
    standIn.slotFrame = free->slotFrame;
    standIn.slotBox = free->slotBox;
    return standIn;
}

std::optional<scene::WalkerPose> EditRoom::focusPose(world::PickId pick, const FocusViewer& viewer) const {
    const std::optional<world::CollectionItem> standIn = asItem(pick);
    if (!standIn) {
        return std::nullopt;
    }
    return focusPoseFor(*standIn, viewer);
}

const world::EditableFixture* EditRoom::fixture(const world::FixtureId& id) const noexcept {
    for (const world::EditableFixture& fixture : m_fixtures) {
        if (fixture.id == id) {
            return &fixture;
        }
    }
    return nullptr;
}

std::optional<EditTarget> EditRoom::fixtureTarget(const world::FixtureId& id) const {
    if (fixture(id) == nullptr) {
        return std::nullopt;
    }
    EditTarget target;
    target.fixture = id;
    return target;
}

std::optional<world::FixtureId> EditRoom::fixtureAt(const core::Ray& ray, float maxDistance) const noexcept {
    std::optional<world::FixtureId> nearest;
    float best = maxDistance;
    for (std::size_t index = 0; index < m_fixtures.size(); ++index) {
        if (m_fixtureBoxes[index].isEmpty()) {
            continue;
        }
        if (const std::optional<float> distance = m_fixtureBoxes[index].intersectRay(ray, best);
            distance && (!nearest || *distance < best)) {
            best = *distance;
            nearest = m_fixtures[index].id;
        }
    }
    // A fixed fixture before it (the hero corner, a hero-wall module) hides it.
    if (nearest && std::any_of(m_fixedBoxes.begin(), m_fixedBoxes.end(), [&](const core::Aabb& box) {
            return !box.isEmpty() && box.intersectRay(ray, best).has_value();
        })) {
        return std::nullopt;
    }
    return nearest;
}

std::optional<core::Aabb> EditRoom::fixtureBoxAt(const world::FixtureId& id,
                                                   const world::FixtureTransform& place) const noexcept {
    for (std::size_t index = 0; index < m_fixtures.size(); ++index) {
        if (m_fixtures[index].id == id) {
            return m_fixtureBoxes[index].transformed(fixtureMove(m_fixtures[index].at, place, m_frame.toScene));
        }
    }
    return std::nullopt;
}

std::optional<world::FixtureTransform> EditRoom::floorAim(const core::Ray& ray) const noexcept {
    const glm::mat4 toRoom = glm::inverse(m_frame.toScene);
    const glm::vec3 origin = glm::vec3(toRoom * glm::vec4(ray.origin, 1.0f));
    const glm::vec3 direction = glm::vec3(toRoom * glm::vec4(ray.direction, 0.0f));
    if (!(direction.y < -1e-4f) || !std::isfinite(origin.y)) {
        return std::nullopt;
    }
    const float t = -origin.y / direction.y;
    if (!(t >= 0.0f) || !std::isfinite(t)) {
        return std::nullopt;
    }
    const glm::vec3 point = origin + t * direction;
    return world::FixtureTransform{
        std::clamp(millimetres(point.x), m_frame.minXMm, std::max(m_frame.minXMm, m_frame.maxXMm)),
        std::clamp(millimetres(point.z), m_frame.minZMm, std::max(m_frame.minZMm, m_frame.maxZMm)), 0};
}

std::optional<world::FixtureTransform> EditRoom::wallAim(const core::Ray& ray) const noexcept {
    const glm::mat4 toRoom = glm::inverse(m_frame.toScene);
    const glm::vec3 origin = glm::vec3(toRoom * glm::vec4(ray.origin, 1.0f));
    const glm::vec3 direction = glm::vec3(toRoom * glm::vec4(ray.direction, 0.0f));
    const float minX = static_cast<float>(m_frame.minXMm) / 1000.0f;
    const float maxX = static_cast<float>(m_frame.maxXMm) / 1000.0f;
    const float minZ = static_cast<float>(m_frame.minZMm) / 1000.0f;
    const float maxZ = static_cast<float>(m_frame.maxZMm) / 1000.0f;
    std::optional<float> nearest;
    const auto consider = [&](float t) {
        if (std::isfinite(t) && t > 0.0f && (!nearest || t < *nearest)) {
            nearest = t;
        }
    };
    if (direction.x < -1e-6f) {
        consider((minX - origin.x) / direction.x);
    } else if (direction.x > 1e-6f) {
        consider((maxX - origin.x) / direction.x);
    }
    if (direction.z < -1e-6f) {
        consider((minZ - origin.z) / direction.z);
    } else if (direction.z > 1e-6f) {
        consider((maxZ - origin.z) / direction.z);
    }
    if (!nearest) {
        return std::nullopt;
    }
    const glm::vec3 point = origin + *nearest * direction;
    return world::FixtureTransform{
        std::clamp(millimetres(point.x), m_frame.minXMm, std::max(m_frame.minXMm, m_frame.maxXMm)),
        std::clamp(millimetres(point.z), m_frame.minZMm, std::max(m_frame.minZMm, m_frame.maxZMm)), 0};
}

glm::vec2 EditRoom::roomDirection(const glm::vec3& direction) const noexcept {
    const glm::vec3 inRoom = glm::vec3(glm::inverse(m_frame.toScene) * glm::vec4(direction, 0.0f));
    return glm::vec2(inRoom.x, inRoom.z);
}

} // namespace rv::app
