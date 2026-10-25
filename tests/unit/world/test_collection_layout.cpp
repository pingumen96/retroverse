// The collection's layout (rv/world/collection_layout.hpp; I3 step 10a) on the golden plans of step 3: every placement
// one item, its transform the prefab's slot socket plus the placement's offset within its bounds, no item intersecting
// another, an occluder or the dressing, the clusters and groups with their counts, stable pick ids equal to the
// instance record's, the pick index built from the layout picking each item through its centre and never through a
// board, and the D-pad navigation built from it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/scene/instance_data.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/slot_navigation.hpp>

#include "plan_scene_support.hpp"

using rv::test::world::goldenPlanNames;
using rv::test::world::layoutOf;
using rv::test::world::PlanCase;
using rv::test::world::planCase;
using rv::world::CollectionItem;
using rv::world::CollectionLayout;
using rv::world::ItemStance;
using rv::world::PickId;

namespace {

/// The plans whose items are checked pairwise (each size once, the largest rooms and a small one).
const std::vector<std::string> kGeometryPlans{"plan-15-seed-102938", "plan-50-seed-102938", "plan-57-seed-7",
                                              "plan-890-seed-102938", "plan-2000-seed-18446744073709551615",
                                              "plan-8000-seed-102938"};

const rv::world::Fixture& fixtureOf(const PlanCase& entry, const rv::world::FixtureId& id) {
    const auto found = std::find_if(entry.plan.fixtures.begin(), entry.plan.fixtures.end(),
                                    [&](const rv::world::Fixture& fixture) { return fixture.id == id; });
    INFO("fixture " << id.toString());
    REQUIRE(found != entry.plan.fixtures.end());
    return *found;
}

const rv::world::Placement& placementOf(const PlanCase& entry, const rv::world::SlotId& slot) {
    const auto found = std::find_if(entry.plan.placements.begin(), entry.plan.placements.end(),
                                    [&](const rv::world::Placement& placement) { return placement.slot == slot; });
    INFO("placement in " << slot.toString());
    REQUIRE(found != entry.plan.placements.end());
    return *found;
}

/// The fixture's prefab built here from the parameters its manifest resolved to (not the layout's own copy).
const rv::geometry::ProceduralPrefab& builtPrefab(const PlanCase& entry, const rv::world::PrefabRef& ref) {
    static std::map<std::pair<std::string, rv::world::PrefabRef>, rv::geometry::ProceduralPrefab> built;
    const auto key = std::make_pair(entry.name, ref);
    if (const auto found = built.find(key); found != built.end()) {
        return found->second;
    }
    const rv::world::ScenePrefab* prefab = entry.data.prefabs.find(ref);
    REQUIRE(prefab != nullptr);
    auto result = rv::geometry::buildPrefab(prefab->generator, prefab->params);
    REQUIRE(result.has_value());
    return built.emplace(key, std::move(*result)).first->second;
}

float maxDifference(const glm::mat4& a, const glm::mat4& b) {
    float difference = 0.0f;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            difference = std::max(difference, std::abs(a[c][r] - b[c][r]));
        }
    }
    return difference;
}

/// The middle of an item's visible face and the unit vector out of it, in the room.
std::pair<glm::vec3, glm::vec3> faceOf(const CollectionItem& item) {
    const rv::core::Aabb& box = item.slotBox;
    const glm::vec3 centre(box.center().x, box.center().y, box.max.z);
    return {glm::vec3(item.slotFrame * glm::vec4(centre, 1.0f)),
            glm::normalize(glm::vec3(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)))};
}

} // namespace

TEST_CASE("world/collection_layout: every placement of a golden plan becomes exactly one item",
          "[world][collection_layout]") {
    for (const std::string& name : goldenPlanNames()) {
        INFO(name);
        const PlanCase& entry = planCase(name);
        const CollectionLayout layout = layoutOf(entry);
        CHECK(layout.room == entry.room);
        CHECK(layout.skipped.empty());
        REQUIRE(layout.items.size() == entry.plan.placements.size());

        std::set<rv::catalog::GameId> games;
        std::set<rv::world::SlotId> slots;
        std::set<PickId> picks;
        for (const CollectionItem& item : layout.items) {
            CHECK(games.insert(item.game).second);
            CHECK(slots.insert(item.slot).second);
            CHECK(item.pickId != rv::world::kNoPick);
            CHECK(picks.insert(item.pickId).second);
            CHECK(placementOf(entry, item.slot).game == item.game);
            // The pick id is the slot's and the instance record's.
            const auto expected = rv::world::slotPickId(entry.data.roomTemplate, item.slot);
            REQUIRE(expected.has_value());
            CHECK(item.pickId == *expected);
            // A face-out item's record shows no spine label (I6 step 4): kNoSpineLabel, the item's flags besides.
            const std::uint32_t stanceFlags =
                item.stance == ItemStance::FaceOut ? rv::scene::instance_flags::kNoSpineLabel : 0u;
            CHECK(item.attributes() == rv::scene::InstanceAttributes{0, 0, item.flags | stanceFlags, item.pickId});
            CHECK((item.flags & rv::scene::instance_flags::kNoSpineLabel) == 0u);
            CHECK(item.instance().model == item.transform);
            CHECK(item.instance().attributes.pickId == item.pickId);
            CHECK(item.pickTarget().id == item.pickId);
            CHECK(item.pickTarget().cluster == item.cluster);
            // The stance follows the section; the flags and the title the snapshot's titles.
            CHECK((item.stance == ItemStance::FaceOut) == rv::world::isFaceOut(item.section));
            const auto title = std::find_if(entry.titles.begin(), entry.titles.end(),
                                            [&](const rv::world::CollectionTitle& t) { return t.game == item.game; });
            const bool available = title != entry.titles.end() && title->available;
            CHECK(item.flags == (available ? 0u : rv::scene::instance_flags::kUnavailable));
            CHECK(item.title == (title != entry.titles.end() ? title->title : std::string()));
            CHECK(item.frontLayerKey == item.game.toString() + "/front");
            CHECK(item.spineLayerKey == item.game.toString() + "/spine");
            // find() and findGame() answer it.
            CHECK(layout.find(item.pickId) == &item);
            CHECK(layout.findGame(item.game) == &item);
        }
        CHECK(layout.find(rv::world::kNoPick) == nullptr);
    }
}

TEST_CASE("world/collection_layout: pick ids are the slot's, stable across plans of one template",
          "[world][collection_layout]") {
    const rv::world::RoomTemplate& roomTemplate = planCase("plan-890-seed-102938").data.roomTemplate;
    const auto room = *rv::world::RoomId::parse("room:snes");
    const auto hwW = *rv::world::FixtureId::make(room, "hw-w");
    const auto id = rv::world::slotPickId(roomTemplate, *rv::world::SlotId::make(hwW, 3, 12));
    REQUIRE(id.has_value());
    // hw-w is the template's first socket: (0 + 1) << 24 | 3 << 14 | 12.
    CHECK(*id == ((1u << 24) | (3u << 14) | 12u));
    CHECK_FALSE(rv::world::slotPickId(roomTemplate,
                                      *rv::world::SlotId::make(*rv::world::FixtureId::make(room, "nosuch"), 0, 0))
                    .has_value());

    // The same slot has the same pick id in every plan; the item standing there may differ.
    std::map<rv::world::SlotId, PickId> seen;
    for (const std::string& name : {"plan-890-seed-102938", "plan-890-seed-7", "plan-2000-seed-102938"}) {
        for (const CollectionItem& item : layoutOf(planCase(name)).items) {
            const auto [it, inserted] = seen.emplace(item.slot, item.pickId);
            CHECK(it->second == item.pickId);
        }
    }
}

TEST_CASE("world/collection_layout: transforms are the prefabs' slot sockets plus offsets within their bounds",
          "[world][collection_layout]") {
    for (const std::string& name : goldenPlanNames()) {
        INFO(name);
        const PlanCase& entry = planCase(name);
        const CollectionLayout layout = layoutOf(entry);
        const rv::world::MediaFamily& family = entry.data.mediaFamily;
        const glm::vec3 itemSize(static_cast<float>(family.itemWidthMm) / 1000.0f,
                                 static_cast<float>(family.itemHeightMm) / 1000.0f,
                                 static_cast<float>(family.itemDepthMm) / 1000.0f);
        const rv::core::Aabb itemBox = rv::core::Aabb::fromCenterHalfExtents(glm::vec3(0.0f), 0.5f * itemSize);
        for (const CollectionItem& item : layout.items) {
            INFO(item.slot.toString());
            const rv::world::Fixture& fixture = fixtureOf(entry, item.slot.fixture());
            const rv::world::Placement& placement = placementOf(entry, item.slot);
            const rv::geometry::ProceduralPrefab& prefab = builtPrefab(entry, fixture.prefab);
            const rv::geometry::Socket* socket = rv::geometry::findSocket(
                prefab.sockets, "Slot_" + std::to_string(item.slot.level()) + "_" + std::to_string(item.slot.index()));
            REQUIRE(socket != nullptr);
            // The slot as the plan places its fixture: the pivot, then the yaw about +Y.
            const glm::mat4 fixtureFrame =
                glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(fixture.xMm),
                                                                      static_cast<float>(fixture.yMm),
                                                                      static_cast<float>(fixture.zMm)) /
                                                                1000.0f),
                            glm::radians(static_cast<float>(fixture.yawCdeg) / 100.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 slot = fixtureFrame * socket->transform;
            CHECK(item.slotBox == socket->bounds);

            // The offsets stay within ADR-0010 §6's bounds: spine-out pushed in by 0 to 4 mm, face-out turned by at
            // most 1.5 degrees.
            if (item.stance == ItemStance::SpineOut) {
                CHECK(placement.insetMm >= 0);
                CHECK(placement.insetMm <= 4);
                CHECK(placement.yawCdeg == 0);
            } else {
                CHECK(placement.insetMm == 0);
                CHECK(std::abs(placement.yawCdeg) <= 150);
            }
            const glm::mat4 offset =
                glm::rotate(glm::translate(glm::mat4(1.0f),
                                           glm::vec3(0.0f, 0.0f, -static_cast<float>(placement.insetMm) / 1000.0f)),
                            glm::radians(static_cast<float>(placement.yawCdeg) / 100.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            CHECK(maxDifference(item.slotFrame, slot * offset) < 2e-5f);
            const auto facing = item.stance == ItemStance::FaceOut ? rv::geometry::SlotFacing::FaceOut
                                                                   : rv::geometry::SlotFacing::SpineOut;
            CHECK(maxDifference(item.transform, item.slotFrame * rv::geometry::slotItemTransform(facing, itemSize)) <
                  1e-6f);

            // The item inside its slot's volume, grown only by what its offset moves it (4 mm deeper, 2 mm for a
            // turn of 1.5 degrees: 62.5 mm x sin 1.5 degrees, 1.64 mm).
            const glm::mat4 toSlot = glm::inverse(slot);
            const glm::vec3 grow = item.stance == ItemStance::SpineOut ? glm::vec3(1e-4f) : glm::vec3(2e-3f, 1e-4f, 2e-3f);
            for (const glm::vec3& corner : rv::test::world::corners(item.transform, itemBox)) {
                const glm::vec3 local = glm::vec3(toSlot * glm::vec4(corner, 1.0f));
                CHECK(local.x >= socket->bounds.min.x - grow.x);
                CHECK(local.x <= socket->bounds.max.x + grow.x);
                CHECK(local.y >= socket->bounds.min.y - grow.y);
                CHECK(local.y <= socket->bounds.max.y + grow.y);
                CHECK(local.z >= socket->bounds.min.z - grow.z - (item.stance == ItemStance::SpineOut ? 4.0001e-3f : 0.0f));
                CHECK(local.z <= socket->bounds.max.z + grow.z);
            }
        }
    }
}

TEST_CASE("world/collection_layout: no item intersects another, an occluder or the dressing",
          "[world][collection_layout]") {
    for (const std::string& name : kGeometryPlans) {
        INFO(name);
        const CollectionLayout layout = layoutOf(planCase(name));
        std::vector<std::pair<rv::core::Aabb, std::size_t>> bounds;
        // Grown by 1 cm, so that neighbours 4 to 12 mm apart are tested too.
        for (std::size_t i = 0; i < layout.items.size(); ++i) {
            bounds.emplace_back(layout.items[i].worldBounds().inflated(0.01f), i);
        }
        std::sort(bounds.begin(), bounds.end(),
                  [](const auto& a, const auto& b) { return a.first.min.x < b.first.min.x; });
        std::size_t pairs = 0;
        for (std::size_t i = 0; i < bounds.size(); ++i) {
            const CollectionItem& a = layout.items[bounds[i].second];
            const auto boxA = rv::test::world::orientedBox(a.slotFrame, a.slotBox);
            for (std::size_t j = i + 1; j < bounds.size() && bounds[j].first.min.x <= bounds[i].first.max.x; ++j) {
                if (!bounds[i].first.intersects(bounds[j].first)) {
                    continue;
                }
                ++pairs;
                const CollectionItem& b = layout.items[bounds[j].second];
                const float depth =
                    rv::test::world::overlapDepth(boxA, rv::test::world::orientedBox(b.slotFrame, b.slotBox));
                INFO(a.slot.toString() << " and " << b.slot.toString() << ": " << depth * 1000.0f << " mm deep");
                CHECK(depth <= 1e-4f);
            }
            // Occluders (boards, sides, walls, furniture, dressing) are touched at most: an item stands on its board. A
            // face-out item turned by its offset (at most 1.5 degrees about the slot's +Y) sweeps one back corner into
            // what it leans on (the wall shelf's back board, the display case's base under its tilted level) by at most
            // 62.5 mm x sin 1.5 + 10 mm x (1 - cos 1.5) = 1.64 mm, inside the board and behind the item.
            const float allowed = a.stance == ItemStance::FaceOut ? 1.7e-3f : 5e-4f;
            for (const rv::core::Aabb& occluder : layout.occluders) {
                if (!occluder.intersects(bounds[i].first)) {
                    continue;
                }
                const float depth = rv::test::world::overlapDepth(boxA, rv::test::world::orientedBox(occluder));
                INFO(a.slot.toString() << " and the occluder " << occluder.min.x << ", " << occluder.min.y << ", "
                                       << occluder.min.z << " .. " << occluder.max.x << ", " << occluder.max.y << ", "
                                       << occluder.max.z << ": " << depth * 1000.0f << " mm deep");
                CHECK(depth <= allowed);
            }
        }
        INFO(pairs << " neighbouring pairs");
        CHECK((layout.items.size() < 2 || pairs > 0));
    }
}

TEST_CASE("world/collection_layout: clusters and groups hold the items in order, with their counts",
          "[world][collection_layout]") {
    for (const std::string& name : goldenPlanNames()) {
        INFO(name);
        const PlanCase& entry = planCase(name);
        const CollectionLayout layout = layoutOf(entry);
        if (layout.items.empty()) {
            CHECK(layout.clusters.empty());
            CHECK(layout.groups.empty());
            continue;
        }
        REQUIRE(layout.groups.size() == 1);
        const rv::world::CollectionGroup& group = layout.groups.front();
        CHECK(group.mediaFamily == entry.data.mediaFamily.id);
        CHECK(group.item == entry.data.mediaFamily.item);
        CHECK(group.firstItem == 0);
        CHECK(group.itemCount == layout.items.size());
        CHECK(group.firstCluster == 0);
        CHECK(group.clusterCount == layout.clusters.size());

        // The clusters partition the items in order; a module's cluster is one of its levels, any other fixture's
        // the whole fixture; each cluster's bounds hold its items.
        std::uint32_t next = 0;
        std::map<std::pair<rv::world::FixtureId, std::uint32_t>, std::uint32_t> placementsPerCluster;
        for (const rv::world::Placement& placement : entry.plan.placements) {
            const rv::world::Fixture& fixture = fixtureOf(entry, placement.slot.fixture());
            const bool module = fixture.role == rv::world::FixtureRole::HeroModule ||
                                fixture.role == rv::world::FixtureRole::SideModule;
            ++placementsPerCluster[{fixture.id, module ? placement.slot.level() : rv::world::kWholeFixtureLevel}];
        }
        CHECK(layout.clusters.size() == placementsPerCluster.size());
        for (std::size_t c = 0; c < layout.clusters.size(); ++c) {
            const rv::world::CollectionCluster& cluster = layout.clusters[c];
            if (c > 0) {
                CHECK(layout.clusters[c - 1].id < cluster.id);
            }
            CHECK(cluster.firstItem == next);
            CHECK(cluster.itemCount == placementsPerCluster[{cluster.fixture, cluster.level}]);
            for (std::uint32_t i = cluster.firstItem; i < cluster.firstItem + cluster.itemCount; ++i) {
                const CollectionItem& item = layout.items[i];
                CHECK(item.cluster == cluster.id);
                CHECK(item.slot.fixture() == cluster.fixture);
                CHECK(cluster.bounds.contains(item.worldBounds()));
                if (i > cluster.firstItem) {
                    CHECK(layout.items[i - 1].slot < item.slot);
                }
            }
            next += cluster.itemCount;
        }
        CHECK(next == layout.items.size());
    }

    // The reference library's room: 56 face-out (44 featured, 12 in the band), 834 spine-out.
    const CollectionLayout reference = layoutOf(planCase("plan-890-seed-102938"));
    std::map<rv::world::Section, std::size_t> sections;
    std::size_t faceOut = 0;
    for (const CollectionItem& item : reference.items) {
        ++sections[item.section];
        faceOut += item.stance == ItemStance::FaceOut ? 1 : 0;
    }
    CHECK(sections[rv::world::Section::Featured] == 44);
    CHECK(sections[rv::world::Section::Band] == 12);
    CHECK(sections[rv::world::Section::Spine] == 834);
    CHECK(faceOut == 56);
}

TEST_CASE("world/collection_layout: the pick index built from the layout picks each item through its centre",
          "[world][collection_layout]") {
    for (const std::string& name : {"plan-50-seed-102938", "plan-890-seed-102938", "plan-2000-seed-7"}) {
        INFO(name);
        const CollectionLayout layout = layoutOf(planCase(name));
        const std::vector<rv::world::PickTarget> targets = layout.pickTargets();
        REQUIRE(targets.size() == layout.items.size());
        auto index = rv::world::PickIndex::build(targets, layout.occluders);
        INFO((index ? std::string("built") : index.error().toString()));
        REQUIRE(index.has_value());
        CHECK(index->targetCount() == layout.items.size());
        CHECK(index->clusterCount() == layout.clusters.size());
        CHECK(index->occluderCount() == layout.occluders.size());
        std::size_t blocked = 0;
        for (const CollectionItem& item : layout.items) {
            INFO(item.slot.toString());
            const auto [face, out] = faceOf(item);
            // Straight at the face's middle from 0.6 m in front of it: that item.
            const rv::core::Ray ray{face + 0.6f * out, -out};
            const rv::world::PickHit hit = index->pick(ray);
            CHECK(hit.id == item.pickId);
            CHECK(std::abs(hit.distance - 0.6f) < 0.002f);
            // Steeply from below the item's level, through the board under it: never that item.
            if (item.stance == ItemStance::SpineOut) {
                const glm::vec3 up = glm::normalize(glm::vec3(item.slotFrame * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));
                const glm::vec3 origin = face + 0.1f * out - 0.7f * up;
                const rv::world::PickHit below = index->pick(rv::core::Ray{origin, face - origin});
                CHECK(below.id != item.pickId);
                blocked += below.id == rv::world::kNoPick ? 1 : 0;
            }
        }
        INFO(blocked << " rays through a board picked nothing");
        // A ray from the doorway's camera along the room picks nothing (no item within 2.5 m).
        CHECK(index->pick(rv::core::Ray{glm::vec3(0.0f, 1.65f, 4.2f), glm::vec3(0.0f, 0.0f, -1.0f)}).id ==
              rv::world::kNoPick);
    }
}

TEST_CASE("world/collection_layout: the navigation runs hold every item and step along the walls",
          "[world][collection_layout]") {
    for (const std::string& name : goldenPlanNames()) {
        INFO(name);
        const CollectionLayout layout = layoutOf(planCase(name));
        std::size_t items = 0;
        std::size_t slots = 0;
        for (const rv::world::NavRun& run : layout.navigation) {
            for (const rv::world::NavFixture& fixture : run.fixtures) {
                for (const rv::world::NavLevel& level : fixture.levels) {
                    for (const rv::world::NavSlot& slot : level.slots) {
                        ++slots;
                        items += slot.item != rv::world::kNoPick ? 1 : 0;
                    }
                }
            }
        }
        CHECK(items == layout.items.size());
        CHECK(slots >= items);
        auto navigation = rv::world::SlotNavigation::build(layout.navigation);
        INFO((navigation ? std::string("built") : navigation.error().toString()));
        REQUIRE(navigation.has_value());
        for (const CollectionItem& item : layout.items) {
            CHECK(navigation->contains(item.pickId));
        }
    }

    // On the reference room, the north wall's run: hw-w, the featured wall shelf, hw-e, left to right; stepping right
    // along hw-w's top spine-out level reaches its next slot's item.
    const CollectionLayout layout = layoutOf(planCase("plan-890-seed-102938"));
    REQUIRE_FALSE(layout.navigation.empty());
    CHECK(layout.navigation.front().fixtures.size() == 3);
    auto navigation = rv::world::SlotNavigation::build(layout.navigation);
    REQUIRE(navigation.has_value());
    const auto room = *rv::world::RoomId::parse("room:snes");
    const auto hwW = *rv::world::FixtureId::make(room, "hw-w");
    const CollectionItem* first = nullptr;
    const CollectionItem* second = nullptr;
    for (const CollectionItem& item : layout.items) {
        if (item.slot == *rv::world::SlotId::make(hwW, 4, 0)) {
            first = &item;
        }
        if (item.slot == *rv::world::SlotId::make(hwW, 4, 1)) {
            second = &item;
        }
    }
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(navigation->step(first->pickId, rv::world::StepDirection::Right) == second->pickId);
    CHECK(navigation->step(second->pickId, rv::world::StepDirection::Left) == first->pickId);
}

TEST_CASE("world/collection_layout: placements the prefab cannot hold are skipped, malformed plans refused",
          "[world][collection_layout]") {
    const PlanCase& entry = planCase("plan-57-seed-102938");
    rv::world::WorldPlan plan = entry.plan;

    // A placement in a level the plan's fixture lacks, and one in a slot its prefab lacks: skipped with the reason.
    const auto room = entry.room;
    const auto hwE = *rv::world::FixtureId::make(room, "hw-e");
    rv::world::Placement beyond;
    beyond.slot = *rv::world::SlotId::make(hwE, 4, 40);
    beyond.game = *rv::catalog::GameId::fromValue(0x1234);
    plan.placements.push_back(beyond);
    for (rv::world::Fixture& fixture : plan.fixtures) {
        if (fixture.id == hwE) {
            for (rv::world::FixtureLevel& level : fixture.levels) {
                if (level.level == 4) {
                    level.slots = 41; // the plan claims slots the prefab does not have
                }
            }
        }
    }
    auto layout = rv::world::collectionLayout(plan, room, entry.data, entry.titles);
    REQUIRE(layout.has_value());
    REQUIRE(layout->skipped.size() == 1);
    CHECK(layout->skipped.front().placement.slot == beyond.slot);
    CHECK_THAT(layout->skipped.front().reason, Catch::Matchers::ContainsSubstring("no such slot"));
    CHECK(layout->items.size() == entry.plan.placements.size());

    // Two games in one slot: refused.
    rv::world::WorldPlan twice = entry.plan;
    rv::world::Placement copy = twice.placements.front();
    copy.game = *rv::catalog::GameId::fromValue(0x4321);
    twice.placements.push_back(copy);
    auto refused = rv::world::collectionLayout(twice, room, entry.data, entry.titles);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::ValidationFailed);

    // Another room: not found.
    auto other = rv::world::collectionLayout(entry.plan, *rv::world::RoomId::parse("room:nes"), entry.data, entry.titles);
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().code == rv::ErrorCode::NotFound);
}

TEST_CASE("world/collection_layout: titles from a room snapshot", "[world][collection_layout]") {
    rv::catalog::RoomSnapshot snapshot;
    rv::catalog::RoomEntry b;
    b.id = *rv::catalog::GameId::fromValue(2);
    b.title = "Beta";
    b.availability = rv::catalog::Availability::Missing;
    rv::catalog::RoomEntry a;
    a.id = *rv::catalog::GameId::fromValue(1);
    a.title = "Alpha";
    snapshot.entries = {b, a};
    const auto titles = rv::world::collectionTitles(snapshot);
    REQUIRE(titles.size() == 2);
    CHECK(titles[0] == rv::world::CollectionTitle{a.id, "Alpha", true});
    CHECK(titles[1] == rv::world::CollectionTitle{b.id, "Beta", false});
}
