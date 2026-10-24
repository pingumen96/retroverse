// The media families of I4 step 2 (rv/world/media_family.hpp, assets/world/media_families.json; ADR-0013 §4): every
// family's item builds from its manifest at the family's size with its label quad and its spine label, every family's
// fixtures build from their manifests (no fallback) and their levels face the family exactly (the side-wall modules
// spine-out, the hero-wall modules spine-out but for one face-out band, the display fixtures face-out), a room of each
// family's system generates and validates at a few sizes with the family's fixtures, the 16-bit cartridges keep the
// template's own fixtures and I3's cover fit, and each family's capacity in the 9 x 10 m room and grown to 16 m (the
// figures docs/i4/step-2.md compares with the reference library). Since I6 step 4 (ADR-0014 §2) the cases and boxes
// have successors standing on their side (the *_v2 families: an upright spine on the item's -X face), which
// systems.json and the shared rooms name; their predecessors stay readable for the rooms that record them.

#include "world_generation_support.hpp"

#include <rv/assets/prefab_manifest.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/building.hpp>
#include <rv/world/capacity.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/media_family.hpp>
#include <rv/world/validation.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using rv::world::LevelFacing;
using rv::world::MediaFamily;
using rv::world::PrefabRef;
using rv::world::WorldData;

namespace {

using rv::test::world::assetRoot;

const rv::world::MediaFamilyRegistry& registry() {
    static const rv::world::MediaFamilyRegistry families = [] {
        auto loaded = rv::world::loadMediaFamilies(assetRoot());
        REQUIRE(loaded.has_value());
        return *loaded;
    }();
    return families;
}

/// A system of each family (systems.json): its room is the family's.
struct FamilyCase {
    std::string_view family;
    std::string_view system;
};
constexpr FamilyCase kFamilies[] = {
    {"cartridge_home_16bit", "snes"},
    {"cartridge_home_8bit", "nes"},
    {"cartridge_home_64bit", "n64"},
    {"cartridge_handheld", "gb"},
    {"case_handheld_v2", "nds"},
    {"disc_case_cd_v2", "psx"},
    {"disc_case_dvd_v2", "ps2"},
    {"big_box_v2", "amiga"},
    {"generic_case_v2", "c64"},
    // The predecessors, as a stored room records them (ADR-0013 §3: the recorded family is authoritative).
    {"case_handheld", "nds"},
    {"disc_case_cd", "psx"},
    {"disc_case_dvd", "ps2"},
    {"big_box", "amiga"},
    {"generic_case", "c64"},
};

/// The families I6 step 4 succeeds, each with its successor.
constexpr std::pair<std::string_view, std::string_view> kSuccessions[] = {
    {"disc_case_cd", "disc_case_cd_v2"}, {"disc_case_dvd", "disc_case_dvd_v2"}, {"case_handheld", "case_handheld_v2"},
    {"big_box", "big_box_v2"},           {"generic_case", "generic_case_v2"},
};

rv::catalog::SystemName systemName(std::string_view name) {
    auto parsed = rv::catalog::SystemName::parse(name);
    REQUIRE(parsed.has_value());
    return *parsed;
}

/// The data of `system`'s room, of its own family or, as a stored room's, of the recorded `family`.
WorldData dataOf(std::string_view system, std::string_view family = {}) {
    rv::world::RoomDataRequest request;
    request.system = systemName(system);
    request.mediaFamily = std::string(family);
    auto data = rv::world::loadWorldData(assetRoot(), request);
    REQUIRE(data.has_value());
    return std::move(*data);
}

/// A recorded family, unless it is the system's own (systems.json's).
std::string_view recordedFamily(const FamilyCase& entry) {
    return entry.family.ends_with("_v2") || entry.family.starts_with("cartridge") ? std::string_view() : entry.family;
}

/// makeSelection()'s selection for `system`'s room.
rv::world::RoomSelection selectionOf(std::string_view system, std::size_t titles, std::uint64_t seed) {
    rv::world::RoomSelection selection = rv::test::world::makeSelection(titles, seed);
    selection.room = *rv::world::RoomId::forSystem(systemName(system));
    for (rv::world::UnplacedGame& game : selection.unplaced) {
        game.room = selection.room;
    }
    return selection;
}

/// The facings of a fixture's levels, measured from its manifest as the generator reads it.
std::vector<LevelFacing> facingsOf(const rv::world::PrefabInfo& info, const MediaFamily& family) {
    std::vector<LevelFacing> facings;
    for (const rv::world::SlotLevelInfo& level : info.levels) {
        facings.push_back(level.facing(family));
    }
    return facings;
}

} // namespace

TEST_CASE("world/families: the registry holds ADR-0013's families, the 16-bit one as I3 had it", "[world][families]") {
    std::vector<std::string> ids;
    for (const MediaFamily& family : registry().families) {
        ids.push_back(family.id);
    }
    CHECK(ids == std::vector<std::string>{"cartridge_home_16bit", "cartridge_home_8bit", "cartridge_home_64bit",
                                          "cartridge_handheld", "case_handheld", "disc_case_cd", "disc_case_dvd",
                                          "big_box", "generic_case", "disc_case_cd_v2", "disc_case_dvd_v2",
                                          "case_handheld_v2", "big_box_v2", "generic_case_v2"});
    const MediaFamily* sixteen = registry().find("cartridge_home_16bit");
    REQUIRE(sixteen != nullptr);
    // I3's fit and the template's own fixtures (ADR-0013 §4's stated exception).
    CHECK(sixteen->coverFit == rv::world::CoverFit::Layer);
    CHECK(sixteen->coverAspect().isLayer());
    CHECK(sixteen->fixtures.empty());
    CHECK(sixteen->labelWidthMm == 101);
    CHECK(sixteen->labelHeightMm == 61);
    const rv::world::RoomTemplate& base = rv::test::world::snesData().roomTemplate;
    CHECK(rv::world::withFamilyFixtures(base, *sixteen) == base);
    // Every other family fits its covers to its label and names its four fixtures.
    for (const MediaFamily& family : registry().families) {
        INFO(family.id);
        if (family.id == "cartridge_home_16bit") {
            continue;
        }
        CHECK(family.coverFit == rv::world::CoverFit::Label);
        CHECK(family.coverAspect() ==
              rv::assets::LabelAspect{static_cast<std::uint32_t>(family.labelWidthMm),
                                      static_cast<std::uint32_t>(family.labelHeightMm)});
        CHECK(family.fixtures.heroModule.has_value());
        CHECK(family.fixtures.sideModule.has_value());
        CHECK(family.fixtures.display.has_value());
        CHECK(family.fixtures.wallShelf.has_value());
    }
    // Every family systems.json names is in the registry.
    auto systems = rv::world::loadSystemsData(assetRoot());
    REQUIRE(systems.has_value());
    for (const rv::world::SystemEntry& entry : systems->systems) {
        INFO(entry.system.str());
        if (entry.kind != rv::world::SystemKind::Excluded) {
            CHECK(registry().find(entry.family) != nullptr);
        }
    }
    CHECK(registry().find(systems->fallback.family) != nullptr);
    // systems.json names no family that has a successor (I6 step 4: new rooms take the successors).
    for (const rv::world::SystemEntry& entry : systems->systems) {
        INFO(entry.system.str());
        if (entry.kind != rv::world::SystemKind::Excluded) {
            CHECK(registry().successorOf(entry.family) == nullptr);
        }
    }
    CHECK(registry().successorOf(systems->fallback.family) == nullptr);
    CHECK(rv::world::kSharedRoomFamily == "generic_case_v2");
    // The shapes the covers are fitted to: a square jewel-case insert, portrait keep cases and boxes.
    CHECK(registry().find("disc_case_cd")->coverAspect().key() == "1x1");
    CHECK(registry().find("disc_case_dvd")->coverAspect().key() == "123x178");
    CHECK(registry().find("big_box")->coverAspect().key() == "18x23");
}

TEST_CASE("world/families: cases and boxes have successors standing on their side, cartridges none",
          "[world][families]") {
    for (const auto& [from, to] : kSuccessions) {
        INFO(from);
        const MediaFamily* old = registry().find(from);
        const MediaFamily* successor = registry().find(to);
        REQUIRE(old != nullptr);
        REQUIRE(successor != nullptr);
        CHECK(registry().successorOf(from) == successor);
        CHECK(registry().latestSuccessor(from) == successor);
        CHECK(registry().latestSuccessor(to) == successor);
        CHECK(successor->succeeds == from);
        CHECK(old->spineFacing == rv::world::SpineFacing::End);
        CHECK(successor->spineFacing == rv::world::SpineFacing::Side);
        // The same item and cover; spine-out on its side its slot is the item's depth wide, its height tall.
        CHECK(successor->itemWidthMm == old->itemWidthMm);
        CHECK(successor->itemHeightMm == old->itemHeightMm);
        CHECK(successor->itemDepthMm == old->itemDepthMm);
        CHECK(successor->labelWidthMm == old->labelWidthMm);
        CHECK(successor->labelHeightMm == old->labelHeightMm);
        CHECK(successor->faceOut == old->faceOut);
        CHECK(successor->spineOut.widthMm == old->itemDepthMm);
        CHECK(successor->spineOut.heightMm == old->itemHeightMm);
        CHECK(successor->spineOut.depthMm == old->itemWidthMm);
        // Face-out, a successor's items stand in its predecessor's display fixtures.
        CHECK(successor->fixtures.display == old->fixtures.display);
        CHECK(successor->fixtures.wallShelf == old->fixtures.wallShelf);
        CHECK(successor->fixtures.heroModule != old->fixtures.heroModule);
        CHECK(successor->fixtures.sideModule != old->fixtures.sideModule);
    }
    for (const std::string_view cartridge :
         {"cartridge_home_16bit", "cartridge_home_8bit", "cartridge_home_64bit", "cartridge_handheld"}) {
        INFO(cartridge);
        CHECK(registry().successorOf(cartridge) == nullptr);
        CHECK(registry().find(cartridge)->spineFacing == rv::world::SpineFacing::End);
        CHECK(registry().find(cartridge)->succeeds.empty());
    }
    CHECK(registry().latestSuccessor("no_such_family") == nullptr);

    // The reader keeps the successions sound: a family succeeding itself, an unknown one, or one succeeded twice.
    const auto parse = [](std::string_view extra) {
        const std::string text = std::string(R"({"schemaVersion": 1, "families": [)") +
                                 R"({"id": "a", "item": {"manifest": "media.case.plain.generic", "kind": "cartridge"},
                                  "itemWidthMm": 130, "itemHeightMm": 180, "itemDepthMm": 15, "labelWidthMm": 118,
                                  "labelHeightMm": 168, "coverFit": "label",
                                  "spineOut": {"widthMm": 15, "heightMm": 130, "depthMm": 180, "gapMm": 4,
                                               "marginMm": 30, "headroomMm": 20},
                                  "faceOut": {"widthMm": 130, "heightMm": 180, "depthMm": 15, "gapMm": 4,
                                              "marginMm": 30, "headroomMm": 20}})" +
                                 std::string(extra) + "]}";
        return rv::world::parseMediaFamilies(text, "test");
    };
    const auto side = [](std::string_view id, std::string_view succeeds, std::string_view slot) {
        return std::string(R"(, {"id": ")") + std::string(id) + R"(", "succeeds": ")" + std::string(succeeds) +
               R"(", "spineFacing": "side",
                  "item": {"manifest": "media.case.plain_side.generic", "kind": "cartridge"},
                  "itemWidthMm": 130, "itemHeightMm": 180, "itemDepthMm": 15, "labelWidthMm": 118,
                  "labelHeightMm": 168, "coverFit": "label", "spineOut": )" +
               std::string(slot) + R"(, "faceOut": {"widthMm": 130, "heightMm": 180, "depthMm": 15, "gapMm": 4,
                                                "marginMm": 30, "headroomMm": 20}})";
    };
    constexpr std::string_view kSideSlot =
        R"({"widthMm": 15, "heightMm": 180, "depthMm": 130, "gapMm": 4, "marginMm": 30, "headroomMm": 20})";
    constexpr std::string_view kEndSlot =
        R"({"widthMm": 15, "heightMm": 130, "depthMm": 180, "gapMm": 4, "marginMm": 30, "headroomMm": 20})";
    auto good = parse(side("b", "a", kSideSlot));
    REQUIRE(good.has_value());
    CHECK(good->latestSuccessor("a")->id == "b");
    auto endSlot = parse(side("b", "a", kEndSlot));
    REQUIRE_FALSE(endSlot.has_value());
    CHECK_THAT(endSlot.error().message, ContainsSubstring("on its side"));
    auto self = parse(side("b", "b", kSideSlot));
    REQUIRE_FALSE(self.has_value());
    CHECK_THAT(self.error().message, ContainsSubstring("cannot succeed itself"));
    auto unknown = parse(side("b", "z", kSideSlot));
    REQUIRE_FALSE(unknown.has_value());
    CHECK_THAT(unknown.error().message, ContainsSubstring("no family z"));
    auto twice = parse(side("b", "a", kSideSlot) + side("c", "a", kSideSlot));
    REQUIRE_FALSE(twice.has_value());
    CHECK_THAT(twice.error().message, ContainsSubstring("has a successor already"));

    // A new room asked for a succeeded family takes its successor; a stored room keeps its own and carries the data
    // of its migration.
    const WorldData stored = dataOf("psx", "disc_case_cd");
    CHECK(stored.mediaFamily.id == "disc_case_cd");
    REQUIRE(stored.successor != nullptr);
    CHECK(stored.successor->mediaFamily.id == "disc_case_cd_v2");
    CHECK(stored.successor->platform.themePack == stored.platform.themePack);
    CHECK(stored.successor->roomTemplate.sideModule == *registry().find("disc_case_cd_v2")->fixtures.sideModule);
    CHECK(stored.successor->successor == nullptr);
    const WorldData fresh = dataOf("psx");
    CHECK(fresh.mediaFamily.id == "disc_case_cd_v2");
    CHECK(fresh.successor == nullptr);
    CHECK(dataOf("snes", "cartridge_home_16bit").successor == nullptr);
}

TEST_CASE("world/families: every family's item builds from its manifest at the family's size", "[world][families]") {
    for (const MediaFamily& family : registry().families) {
        INFO(family.id);
        auto manifest =
            rv::assets::loadPrefabManifest(rv::assets::prefabManifestPath(assetRoot(), family.item.manifest));
        REQUIRE(manifest.has_value());
        // The size, the label quad (the Label socket) and the build itself (the spine label within its limits).
        auto check = rv::world::checkItemManifest(family, *manifest);
        CHECK(check.has_value());
        if (!check) {
            FAIL(check.error().toString());
        }
        auto prefab = rv::geometry::buildPrefab(family.item.kind, manifest->runtime.params);
        REQUIRE(prefab.has_value());
        CHECK(rv::geometry::findPart(*prefab, "plastic_shell") != nullptr);
        CHECK(rv::geometry::findPart(*prefab, "label") != nullptr);
        CHECK(rv::geometry::findPart(*prefab, "spine") != nullptr);
        const rv::geometry::Socket* spine = rv::geometry::findSocket(prefab->sockets, "Spine");
        REQUIRE(spine != nullptr);
        // The spine label keeps the spine texture's 40 x 256 aspect within 1%, and lies on the item's top face (on its
        // -X face, upright, for a family standing on its side).
        const glm::vec3 spineSize = spine->bounds.max - spine->bounds.min;
        CHECK(spineSize.x / spineSize.y > 0.99f * 40.0f / 256.0f);
        CHECK(spineSize.x / spineSize.y < 1.01f * 40.0f / 256.0f);
        CHECK(spineSize.x * 1000.0f <= static_cast<float>(family.itemDepthMm));
        const bool side = family.spineFacing == rv::world::SpineFacing::Side;
        CHECK(spineSize.y * 1000.0f <= static_cast<float>(side ? family.itemHeightMm : family.itemWidthMm));
        const glm::vec3 out(spine->transform[2]);
        CHECK(glm::dot(out, side ? glm::vec3(-1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f)) > 0.999f);
    }
    // A label that is not the manifest's is refused.
    MediaFamily wrong = *registry().find("disc_case_cd");
    wrong.labelHeightMm = 119;
    auto manifest = rv::assets::loadPrefabManifest(rv::assets::prefabManifestPath(assetRoot(), wrong.item.manifest));
    REQUIRE(manifest.has_value());
    auto refused = rv::world::checkItemManifest(wrong, *manifest);
    REQUIRE_FALSE(refused.has_value());
    CHECK_THAT(refused.error().message, ContainsSubstring("label is 120 x 120 mm"));
    // A family standing on its side whose manifest's spine lies on the top face is refused.
    MediaFamily sideways = *registry().find("disc_case_cd");
    sideways.spineFacing = rv::world::SpineFacing::Side;
    auto topSpine = rv::world::checkItemManifest(sideways, *manifest);
    REQUIRE_FALSE(topSpine.has_value());
    CHECK_THAT(topSpine.error().message, ContainsSubstring("-X face"));
}

TEST_CASE("world/families: every family's fixtures build and their levels face the family exactly",
          "[world][families]") {
    auto prefabFamilies = rv::assets::loadPrefabFamilies(assetRoot());
    REQUIRE(prefabFamilies.has_value());
    const rv::world::RoomTemplate& base = rv::test::world::snesData().roomTemplate;
    for (const MediaFamily& family : registry().families) {
        INFO(family.id);
        const rv::world::RoomTemplate roomTemplate = rv::world::withFamilyFixtures(base, family);
        const auto measure = [&](const PrefabRef& ref) {
            INFO(ref.manifest);
            auto info = rv::world::readPrefabInfo(assetRoot(), *prefabFamilies, ref);
            REQUIRE(info.has_value());
            CHECK(info->resolvedManifest == ref.manifest); // its own manifest, no fallback
            return *info;
        };
        // The side-wall module: every level spine-out.
        const rv::world::PrefabInfo side = measure(roomTemplate.sideModule);
        REQUIRE_FALSE(side.levels.empty());
        for (const LevelFacing facing : facingsOf(side, family)) {
            CHECK(facing == LevelFacing::SpineOut);
        }
        // The hero-wall module: one face-out band, the other levels spine-out.
        const rv::world::PrefabInfo hero = measure(roomTemplate.heroModule);
        std::size_t band = 0;
        for (const LevelFacing facing : facingsOf(hero, family)) {
            CHECK(facing != LevelFacing::Other);
            band += facing == LevelFacing::FaceOut ? 1 : 0;
        }
        CHECK(band == 1);
        CHECK(hero.slots(family, LevelFacing::SpineOut) > 0);
        // The display fixtures: every level face-out.
        std::set<std::string> kinds;
        for (const std::string& socketId : roomTemplate.displayOrder) {
            const rv::world::TemplateSocket* socket = roomTemplate.socket(socketId);
            REQUIRE(socket != nullptr);
            REQUIRE(socket->prefab.has_value());
            kinds.insert(socket->prefab->kind);
            const rv::world::PrefabInfo display = measure(*socket->prefab);
            REQUIRE_FALSE(display.levels.empty());
            for (const LevelFacing facing : facingsOf(display, family)) {
                CHECK(facing == LevelFacing::FaceOut);
            }
            // Within its socket.
            CHECK(display.widthMm() <= socket->widthMm);
            CHECK(display.depthMm() <= socket->depthMm);
        }
        CHECK(kinds == std::set<std::string>{"display_case", "wall_shelf"});
        // The modules fit the template's 0.9 x 0.4 m sockets.
        CHECK(side.widthMm() <= 900);
        CHECK(side.depthMm() <= 400);
        CHECK(hero.widthMm() <= 900);
        CHECK(hero.depthMm() <= 400);
        if (!family.fixtures.empty()) {
            CHECK(roomTemplate.heroModule == *family.fixtures.heroModule);
            CHECK(roomTemplate.sideModule == *family.fixtures.sideModule);
        }
    }
}

TEST_CASE("world/families: a room of each family generates and validates at a few sizes", "[world][families]") {
    for (const FamilyCase& entry : kFamilies) {
        INFO(entry.system << " " << entry.family);
        const WorldData data = dataOf(entry.system, recordedFamily(entry));
        REQUIRE(data.mediaFamily.id == entry.family);
        const MediaFamily& family = data.mediaFamily;
        for (const std::size_t titles : {std::size_t{1}, std::size_t{40}, std::size_t{700}, std::size_t{2600}}) {
            INFO(titles);
            rv::world::GenerationInput input;
            input.library = rv::test::world::library();
            input.selection = selectionOf(entry.system, titles, 4000 + titles);
            input.config = rv::test::world::roomConfig(102938); // a room of its own at any size
            auto result = rv::world::generate(input, data);
            REQUIRE(result.has_value());
            for (const rv::world::ValidationFinding& finding : result->validation.findings) {
                if (finding.severity == rv::world::Severity::Hard) {
                    UNSCOPED_INFO(finding.rule << " " << finding.subject << ": " << finding.message);
                }
            }
            CHECK(result->validation.passed());
            const rv::world::WorldPlan& plan = result->plan;
            REQUIRE(plan.rooms.size() == 1);
            CHECK(plan.rooms.front().mediaFamily == entry.family);
            // Every title placed or unplaced for capacity; the fixtures the family's own.
            std::size_t capacity = 0;
            for (const rv::world::UnplacedGame& game : plan.unplaced) {
                capacity += game.reason == rv::world::UnplacedReason::Capacity ? 1 : 0;
            }
            CHECK(plan.placements.size() + capacity == titles);
            for (const rv::world::Fixture& fixture : plan.fixtures) {
                if (fixture.role == rv::world::FixtureRole::HeroModule) {
                    CHECK(fixture.prefab == data.roomTemplate.heroModule);
                } else if (fixture.role == rv::world::FixtureRole::SideModule) {
                    CHECK(fixture.prefab == data.roomTemplate.sideModule);
                }
                if (!family.fixtures.empty() && (fixture.role == rv::world::FixtureRole::HeroModule ||
                                                 fixture.role == rv::world::FixtureRole::SideModule ||
                                                 fixture.role == rv::world::FixtureRole::Display)) {
                    const bool own = fixture.prefab == *family.fixtures.heroModule ||
                                     fixture.prefab == *family.fixtures.sideModule ||
                                     fixture.prefab == *family.fixtures.display ||
                                     fixture.prefab == *family.fixtures.wallShelf;
                    CHECK(own);
                }
            }
        }
    }
}

TEST_CASE("world/families: each family's capacity in the 9 x 10 m room and grown to 16 m", "[world][families]") {
    struct Row {
        std::string_view system;
        std::string_view family;   ///< a recorded family (a predecessor); empty: the system's own
        std::uint64_t display = 0; ///< D
        std::uint64_t band = 0;    ///< B
        std::uint64_t heroSpine = 0; ///< 2 Sh
        std::uint64_t side = 0;    ///< S
        std::uint64_t base = 0;    ///< the titles the 9 x 10 m room holds with the reserve
        std::uint64_t grown = 0;   ///< at the template's limit, 16 m
    };
    // The figures of docs/i4/step-2.md's table, and of docs/i6/step-4.md's for the successors.
    const Row rows[] = {
        {"snes", {}, 44, 12, 396, 231, 3205, 5615},             // cartridge_home_16bit, I3's
        {"nes", {}, 44, 12, 396, 264, 3606, 6361},              // cartridge_home_8bit
        {"n64", {}, 44, 12, 504, 288, 3993, 6998},              // cartridge_home_64bit
        {"gb", {}, 88, 24, 1452, 792, 11001, 19266},            // cartridge_handheld
        {"nds", "case_handheld", 44, 12, 528, 352, 4793, 8466}, // case_handheld
        {"psx", "disc_case_cd", 38, 10, 672, 392, 5398, 9488},  // disc_case_cd
        {"ps2", "disc_case_dvd", 38, 10, 352, 308, 4097, 7311}, // disc_case_dvd
        {"amiga", "big_box", 28, 8, 96, 80, 1088, 1923},        // big_box
        {"c64", "generic_case", 38, 10, 410, 328, 4391, 7813},  // generic_case
        {"nds", {}, 44, 12, 528, 308, 4257, 7471},              // case_handheld_v2
        {"psx", {}, 38, 10, 784, 448, 6177, 10852},             // disc_case_cd_v2
        {"ps2", {}, 38, 10, 352, 220, 3026, 5321},              // disc_case_dvd_v2
        {"amiga", {}, 28, 8, 96, 64, 893, 1561},                // big_box_v2
        {"c64", {}, 38, 10, 410, 246, 3393, 5960},              // generic_case_v2
    };
    for (const Row& row : rows) {
        INFO(row.system << " " << row.family);
        const WorldData data = dataOf(row.system, row.family);
        auto capacity = rv::world::collectionCapacity(data.roomTemplate, data.prefabs, data.mediaFamily);
        REQUIRE(capacity.has_value());
        rv::world::CollectionCapacity unGrown = *capacity;
        unGrown.sideSockets.resize(1);
        const std::uint64_t base = rv::world::countCollection(unGrown, 1'000'000, 150).placed;
        const std::uint64_t grown = rv::world::countCollection(*capacity, 1'000'000, 150).placed;
        UNSCOPED_INFO(data.mediaFamily.id << ": D " << capacity->displaySlots << ", B " << capacity->bandSlots
                                          << ", 2 Sh " << capacity->heroSpineSlots << ", S "
                                          << capacity->sideModuleSlots << ", 9 x 10 m " << base << ", 16 m "
                                          << grown);
        CHECK(capacity->displaySlots == row.display);
        CHECK(capacity->bandSlots == row.band);
        CHECK(capacity->heroSpineSlots == row.heroSpine);
        CHECK(capacity->sideModuleSlots == row.side);
        CHECK(base == row.base);
        CHECK(grown == row.grown);
    }
}
