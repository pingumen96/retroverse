// A walk through the generated room (I3 step 10b; the roadmap's "a scripted walk reaches every shelf section and never
// enters a fixture"), GPU-free: the room of the 50-title golden plan (snes-room's size: the display cases, the wall
// shelf and the hero wall's modules) and of the 890-title one (three side-wall modules too), its collision world from
// the real prefab geometry (scene::buildCollisionWorld of toSceneDescription's scene, as retroverse.exe builds it), the
// first-person controller starting at the doorway's camera and walking, frame by frame at 60 Hz, along a path found on
// a 0.1 m grid of the floor (the cells where the walker's capsule fits) to a point 0.9 m in front of each shelf section
// (every fixture holding items), one after the other: every section reached, the reticle there, aimed at the section's
// middle item, picking an item of it (world::PickIndex over the layout's targets and occluders), and the capsule never
// in a collider.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/assets/material_library.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/scene_geometry.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>

namespace fs = std::filesystem;

namespace {

constexpr float kFixedDt = 0.016667f;
constexpr float kCell = 0.1f;
/// How far in front of a section's middle item the walker stops.
constexpr float kStandOff = 0.9f;
/// Close enough to a waypoint to take the next one, and to the section's stand point.
constexpr float kWaypointReach = 0.06f;
constexpr float kStandReach = 0.15f;

fs::path sourceDir() {
    return rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR);
}

struct Room {
    rv::world::WorldPlan plan;
    rv::scene::SceneDescription scene;
    rv::world::CollectionLayout layout;
    rv::scene::CollisionWorld world;
};

Room roomOf(const std::string& golden) {
    Room room;
    auto text = rv::core::readTextFile(sourceDir() / "tests" / "fixtures" / "world" / "golden" / (golden + ".json"));
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, golden);
    REQUIRE(plan.has_value());
    room.plan = std::move(*plan);
    const rv::world::RoomId id = room.plan.rooms.front().id;
    const fs::path assets = sourceDir() / "assets";
    auto data = rv::world::loadSceneData(assets, room.plan, id);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(room.plan, id, *data);
    REQUIRE(scene.has_value());
    room.scene = std::move(*scene);
    auto layout = rv::world::collectionLayout(room.plan, id, *data, {});
    REQUIRE(layout.has_value());
    room.layout = std::move(*layout);
    auto materials = rv::assets::MaterialLibrary::loadFromFile(assets / "materials" / "base" / "materials.json");
    REQUIRE(materials.has_value());
    room.world = rv::scene::buildCollisionWorld(rv::scene::buildSceneGeometry(room.scene, *materials));
    return room;
}

/// The floor's cells where the capsule fits, searched breadth first.
class FloorGrid {
public:
    FloorGrid(const Room& room, const rv::scene::Capsule& capsule) : m_world(room.world), m_capsule(capsule) {
        const rv::world::Room& bounds = room.plan.rooms.front();
        m_min = glm::vec2(static_cast<float>(bounds.minXMm) / 1000.0f, static_cast<float>(bounds.minZMm) / 1000.0f);
        m_size = glm::ivec2(static_cast<int>(std::ceil(static_cast<float>(bounds.maxXMm - bounds.minXMm) / 100.0f)),
                            static_cast<int>(std::ceil(static_cast<float>(bounds.maxZMm - bounds.minZMm) / 100.0f)));
        m_free.resize(static_cast<std::size_t>(m_size.x) * static_cast<std::size_t>(m_size.y));
        for (int z = 0; z < m_size.y; ++z) {
            for (int x = 0; x < m_size.x; ++x) {
                m_free[index({x, z})] = !m_world.overlaps(feetOf({x, z}), m_capsule);
            }
        }
    }

    [[nodiscard]] glm::vec3 feetOf(glm::ivec2 cell) const {
        return {m_min.x + (static_cast<float>(cell.x) + 0.5f) * kCell, 0.0f,
                m_min.y + (static_cast<float>(cell.y) + 0.5f) * kCell};
    }

    [[nodiscard]] std::optional<glm::ivec2> cellOf(const glm::vec3& feet) const {
        const glm::ivec2 cell(static_cast<int>(std::floor((feet.x - m_min.x) / kCell)),
                              static_cast<int>(std::floor((feet.z - m_min.y) / kCell)));
        if (cell.x < 0 || cell.y < 0 || cell.x >= m_size.x || cell.y >= m_size.y) {
            return std::nullopt;
        }
        return cell;
    }

    /// The free cell nearest to `feet` (within 0.5 m), if any.
    [[nodiscard]] std::optional<glm::ivec2> freeCellNear(const glm::vec3& feet) const {
        const std::optional<glm::ivec2> centre = cellOf(feet);
        if (!centre) {
            return std::nullopt;
        }
        std::optional<glm::ivec2> best;
        float bestDistance = std::numeric_limits<float>::max();
        for (int dz = -5; dz <= 5; ++dz) {
            for (int dx = -5; dx <= 5; ++dx) {
                const glm::ivec2 cell = *centre + glm::ivec2(dx, dz);
                if (!inside(cell) || !m_free[index(cell)]) {
                    continue;
                }
                const glm::vec3 at = feetOf(cell);
                const float distance = glm::length(glm::vec2(at.x - feet.x, at.z - feet.z));
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = cell;
                }
            }
        }
        return best;
    }

    /// The cells of a shortest 8-connected path from `from` to `to` (both free), `to` last; empty when there is none.
    [[nodiscard]] std::vector<glm::ivec2> path(glm::ivec2 from, glm::ivec2 to) const {
        std::vector<int> previous(m_free.size(), -1);
        std::vector<bool> seen(m_free.size(), false);
        std::deque<glm::ivec2> queue{from};
        seen[index(from)] = true;
        while (!queue.empty()) {
            const glm::ivec2 cell = queue.front();
            queue.pop_front();
            if (cell == to) {
                break;
            }
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const glm::ivec2 next = cell + glm::ivec2(dx, dz);
                    if ((dx == 0 && dz == 0) || !inside(next) || seen[index(next)] || !m_free[index(next)]) {
                        continue;
                    }
                    // A diagonal step only between two free neighbours (no corner cut).
                    if (dx != 0 && dz != 0 &&
                        (!m_free[index(cell + glm::ivec2(dx, 0))] || !m_free[index(cell + glm::ivec2(0, dz))])) {
                        continue;
                    }
                    seen[index(next)] = true;
                    previous[index(next)] = static_cast<int>(index(cell));
                    queue.push_back(next);
                }
            }
        }
        if (!seen[index(to)]) {
            return {};
        }
        std::vector<glm::ivec2> cells;
        for (int at = static_cast<int>(index(to)); at >= 0; at = previous[static_cast<std::size_t>(at)]) {
            cells.emplace_back(at % m_size.x, at / m_size.x);
            if (at == static_cast<int>(index(from))) {
                break;
            }
        }
        std::reverse(cells.begin(), cells.end());
        return cells;
    }

private:
    [[nodiscard]] bool inside(glm::ivec2 cell) const noexcept {
        return cell.x >= 0 && cell.y >= 0 && cell.x < m_size.x && cell.y < m_size.y;
    }
    [[nodiscard]] std::size_t index(glm::ivec2 cell) const noexcept {
        return static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(m_size.x) + static_cast<std::size_t>(cell.x);
    }

    const rv::scene::CollisionWorld& m_world;
    rv::scene::Capsule m_capsule;
    glm::vec2 m_min{0.0f};
    glm::ivec2 m_size{0};
    std::vector<bool> m_free;
};

/// One shelf section: a fixture holding items, its middle item.
struct Section {
    std::string fixture;
    const rv::world::CollectionItem* item = nullptr;
};

std::vector<Section> sectionsOf(const Room& room) {
    std::map<std::string, std::vector<const rv::world::CollectionItem*>> byFixture;
    for (const rv::world::CollectionItem& item : room.layout.items) {
        byFixture[item.slot.fixture().toString()].push_back(&item);
    }
    std::vector<Section> sections;
    for (const auto& [fixture, items] : byFixture) {
        sections.push_back(Section{fixture, items[items.size() / 2]});
    }
    return sections;
}

/// Walks `room` from its camera to every section (see the file comment).
void walkEverySection(const Room& room) {
    rv::scene::FirstPersonController walker;
    const rv::scene::FirstPersonSettings settings = walker.settings();
    const rv::scene::CameraSetup& start = room.scene.camera;
    walker.teleport(start.position - glm::vec3(0.0f, settings.eyeHeight, 0.0f),
                    rv::core::degreesToRadians(start.yawDeg), rv::core::degreesToRadians(start.pitchDeg));
    rv::scene::Camera camera;
    camera.setLens(start.lens);
    const FloorGrid grid(room, settings.capsule);
    auto index = rv::world::PickIndex::build(room.layout.pickTargets(), room.layout.occluders);
    REQUIRE(index.has_value());

    const std::vector<Section> sections = sectionsOf(room);
    REQUIRE_FALSE(sections.empty());
    for (const Section& section : sections) {
        INFO("section " << section.fixture);
        const rv::world::CollectionItem& item = *section.item;
        const glm::vec3 local(item.slotBox.center().x, item.slotBox.center().y, item.slotBox.max.z);
        const glm::vec3 face(item.slotFrame * glm::vec4(local, 1.0f));
        glm::vec3 out(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
        out.y = 0.0f;
        out = glm::normalize(out);
        const glm::vec3 stand(face.x + kStandOff * out.x, 0.0f, face.z + kStandOff * out.z);
        const std::optional<glm::ivec2> goal = grid.freeCellNear(stand);
        const std::optional<glm::ivec2> here = grid.freeCellNear(walker.feetPosition());
        REQUIRE(goal.has_value());
        REQUIRE(here.has_value());
        const std::vector<glm::ivec2> cells = grid.path(*here, *goal);
        INFO("path of " << cells.size() << " cell(s)");
        REQUIRE_FALSE(cells.empty());

        // The walk: towards each cell of the path in turn, walking forward (the collision applies), then the last
        // stretch to the stand point itself.
        std::vector<glm::vec3> waypoints;
        for (const glm::ivec2 cell : cells) {
            waypoints.push_back(grid.feetOf(cell));
        }
        std::size_t next = 0;
        std::uint64_t frames = 0;
        while (next < waypoints.size() && frames < 20000) {
            const glm::vec3 feet = walker.feetPosition();
            const glm::vec2 toward(waypoints[next].x - feet.x, waypoints[next].z - feet.z);
            const float reach = next + 1 == waypoints.size() ? kWaypointReach / 2.0f : kWaypointReach;
            if (glm::length(toward) < reach) {
                ++next;
                continue;
            }
            walker.teleport(feet, std::atan2(toward.x, -toward.y), walker.pitch());
            rv::input::ActionState actions;
            // Slow down on the last few centimetres so as not to overshoot (the walker's speed is 2.5 m/s).
            const float step = settings.walkSpeed * kFixedDt;
            actions.setAxis(rv::input::Action::MoveForward, std::min(1.0f, glm::length(toward) / step));
            walker.update(actions, kFixedDt, camera, &room.world);
            REQUIRE_FALSE(room.world.overlaps(walker.feetPosition(), settings.capsule));
            ++frames;
        }
        const glm::vec3 feet = walker.feetPosition();
        CHECK(glm::length(glm::vec2(feet.x - stand.x, feet.z - stand.z)) <= kStandReach + kCell);

        // There, the reticle on the section's middle item picks an item of the section.
        const glm::vec3 eye = feet + glm::vec3(0.0f, settings.eyeHeight, 0.0f);
        const glm::vec3 look = face - eye;
        walker.teleport(feet, std::atan2(look.x, -look.z),
                        std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)));
        walker.update(rv::input::ActionState{}, 0.0f, camera, &room.world);
        const rv::world::PickHit hit = index->pick(camera.rayFromNdc(glm::vec2(0.0f)));
        REQUIRE(hit.id != rv::world::kNoPick);
        const rv::world::CollectionItem* picked = room.layout.find(hit.id);
        REQUIRE(picked != nullptr);
        CHECK(picked->slot.fixture() == item.slot.fixture());
    }
}

} // namespace

TEST_CASE("integration/room_walk: a walk from the doorway reaches every shelf section of the room, never in a fixture",
          "[integration][world]") {
    for (const std::string golden : {"plan-50-seed-102938", "plan-890-seed-102938"}) {
        INFO(golden);
        const Room room = roomOf(golden);
        walkEverySection(room);
    }
}
