#pragma once

// What the plan-to-scene tests share (I3 step 10a): the golden plans of step 3 (tests/fixtures/world/golden) read
// back, their scene data from the source tree's assets (read once per plan), invented titles for their games, and
// geometry checks (an oriented box against another, a box's corners).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::test::world {

inline const std::filesystem::path& sourceAssetRoot() {
    static const std::filesystem::path root = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return root;
}

inline std::filesystem::path goldenPlanDir() {
    return rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "golden";
}

/// The golden plans' names (without ".json"), as step 3 wrote them.
inline const std::vector<std::string>& goldenPlanNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> result;
        for (const char* titles : {"0", "1", "15", "50", "56", "57", "890", "2000"}) {
            for (const char* seed : {"102938", "7", "18446744073709551615"}) {
                result.push_back(std::string("plan-") + titles + "-seed-" + seed);
            }
        }
        result.push_back("plan-8000-seed-102938");
        return result;
    }();
    return names;
}

/// A golden plan with its scene data and titles, read once per name: its first room (a chain's head) alone.
struct PlanCase {
    std::string name;
    rv::world::WorldPlan plan;
    rv::world::RoomId room;
    rv::world::SceneData data;
    std::vector<rv::world::CollectionTitle> titles;
};

/// Invented titles for the plan's placed games: "Title <n>" by the games' order, every seventh game unavailable and
/// every thirteenth missing from the titles (also unavailable: the snapshot lacks it).
inline std::vector<rv::world::CollectionTitle> inventedTitles(const rv::world::WorldPlan& plan) {
    std::vector<rv::catalog::GameId> games;
    for (const rv::world::Placement& placement : plan.placements) {
        games.push_back(placement.game);
    }
    std::sort(games.begin(), games.end());
    std::vector<rv::world::CollectionTitle> titles;
    for (std::size_t i = 0; i < games.size(); ++i) {
        if (i % 13 == 12) {
            continue;
        }
        titles.push_back(rv::world::CollectionTitle{games[i], "Title " + std::to_string(i + 1), i % 7 != 6});
    }
    return titles;
}

inline const PlanCase& planCase(const std::string& name) {
    static std::map<std::string, PlanCase> cases;
    if (const auto found = cases.find(name); found != cases.end()) {
        return found->second;
    }
    auto text = rv::core::readTextFile(goldenPlanDir() / (name + ".json"));
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, name);
    REQUIRE(plan.has_value());
    REQUIRE_FALSE(plan->rooms.empty());
    PlanCase entry;
    entry.name = name;
    entry.room = plan->rooms.front().id;
    // A chain's head alone (plan-8000's room:snes, generator 1.5: ADR-0015 §7), as a plan of that one room.
    const rv::world::RoomId& head = entry.room;
    std::erase_if(plan->rooms, [&](const rv::world::Room& room) { return room.id != head; });
    std::erase_if(plan->fixtures, [&](const rv::world::Fixture& fixture) { return fixture.id.room() != head; });
    std::erase_if(plan->placements,
                  [&](const rv::world::Placement& placement) { return placement.slot.fixture().room() != head; });
    std::erase_if(plan->dressing,
                  [&](const rv::world::DressedLevel& dressed) { return dressed.level.fixture().room() != head; });
    std::erase_if(plan->lights, [&](const rv::world::PlanLight& light) { return light.id.room() != head; });
    std::erase_if(plan->unplaced, [&](const rv::world::UnplacedGame& game) { return game.room != head; });
    auto data = rv::world::loadSceneData(sourceAssetRoot(), *plan, entry.room);
    INFO((data ? std::string("loaded") : data.error().toString()));
    REQUIRE(data.has_value());
    entry.data = std::move(*data);
    entry.titles = inventedTitles(*plan);
    entry.plan = std::move(*plan);
    return cases.emplace(name, std::move(entry)).first->second;
}

inline rv::world::CollectionLayout layoutOf(const PlanCase& entry) {
    auto layout = rv::world::collectionLayout(entry.plan, entry.room, entry.data, entry.titles);
    INFO((layout ? std::string("built") : layout.error().toString()));
    REQUIRE(layout.has_value());
    return std::move(*layout);
}

/// A box (`box` in `frame`, a rigid transform) as a centre, three unit axes and half extents.
struct OrientedBox {
    glm::vec3 centre{0.0f};
    std::array<glm::vec3, 3> axes{};
    glm::vec3 half{0.0f};
};

inline OrientedBox orientedBox(const glm::mat4& frame, const rv::core::Aabb& box) {
    OrientedBox result;
    result.centre = glm::vec3(frame * glm::vec4(box.center(), 1.0f));
    for (int i = 0; i < 3; ++i) {
        result.axes[static_cast<std::size_t>(i)] = glm::normalize(glm::vec3(frame[i]));
    }
    result.half = box.halfExtents();
    return result;
}

inline OrientedBox orientedBox(const rv::core::Aabb& box) {
    return orientedBox(glm::mat4(1.0f), box);
}

/// How deep two oriented boxes overlap: the least overlap along the 15 axes of the separating axis theorem, in meters
/// (0 or less when they are apart or only touch).
inline float overlapDepth(const OrientedBox& a, const OrientedBox& b) {
    const auto radius = [](const OrientedBox& box, const glm::vec3& axis) {
        return box.half.x * std::abs(glm::dot(box.axes[0], axis)) + box.half.y * std::abs(glm::dot(box.axes[1], axis)) +
               box.half.z * std::abs(glm::dot(box.axes[2], axis));
    };
    std::vector<glm::vec3> axes;
    for (const glm::vec3& axis : a.axes) {
        axes.push_back(axis);
    }
    for (const glm::vec3& axis : b.axes) {
        axes.push_back(axis);
    }
    for (const glm::vec3& u : a.axes) {
        for (const glm::vec3& v : b.axes) {
            const glm::vec3 cross = glm::cross(u, v);
            if (glm::length(cross) > 1e-4f) {
                axes.push_back(glm::normalize(cross));
            }
        }
    }
    const glm::vec3 between = b.centre - a.centre;
    float depth = std::numeric_limits<float>::max();
    for (const glm::vec3& axis : axes) {
        const float overlap = radius(a, axis) + radius(b, axis) - std::abs(glm::dot(between, axis));
        depth = std::min(depth, overlap);
    }
    return depth;
}

/// Whether two oriented boxes overlap by more than `tolerance` meters: boxes that only touch, or overlap by less, do
/// not count.
inline bool overlaps(const OrientedBox& a, const OrientedBox& b, float tolerance) {
    return overlapDepth(a, b) > tolerance;
}

/// The eight corners of `box` through `transform`.
inline std::array<glm::vec3, 8> corners(const glm::mat4& transform, const rv::core::Aabb& box) {
    std::array<glm::vec3, 8> result{};
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 local((i & 1) != 0 ? box.max.x : box.min.x, (i & 2) != 0 ? box.max.y : box.min.y,
                              (i & 4) != 0 ? box.max.z : box.min.z);
        result[static_cast<std::size_t>(i)] = glm::vec3(transform * glm::vec4(local, 1.0f));
    }
    return result;
}

} // namespace rv::test::world
