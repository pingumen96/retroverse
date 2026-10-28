// The media families of I4 step 2 (rv/world/media_family.hpp, assets/world/media_families.json; ADR-0013 §4): every
// family's item builds from its manifest at the family's size with its label quad and its spine label, every family's
// fixtures build from their manifests (no fallback) and their levels face the family exactly (the side-wall modules
// spine-out, the hero-wall modules spine-out but for one face-out band, the display fixtures face-out), a room of each
// family's system generates and validates at a few sizes with the family's fixtures, the 16-bit cartridges keep the
// template's own fixtures and I3's cover fit, and each family's capacity in the 9 x 10 m room and grown to 16 m (the
// figures docs/i4/step-2.md compares with the reference library). Since I6 step 4 (ADR-0014 §2) the cases and boxes
// have successors standing on their side (the *_v2 families: an upright spine on the item's -X face), which
// systems.json and the shared rooms name; their predecessors stay readable for the rooms that record them. Since I6
// step 6 (ADR-0014 §2, §6) the arcade room's family, arcade_cabinet, succeeds generic_case there: rows of cabinets
// whose screens face out in every fixture, the modules' levels the spine section's by their role.

#include "world_generation_support.hpp"

#include <rv/assets/material_library.hpp>
#include <rv/assets/prefab_manifest.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/building.hpp>
#include <rv/world/capacity.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/media_family.hpp>
#include <rv/world/validation.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
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
    {"arcade_cabinet", "mame"}, // I6 step 6: the arcade systems' family, its rows of cabinets
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
    return entry.family.ends_with("_v2") || entry.family.starts_with("cartridge") || entry.family == "arcade_cabinet"
               ? std::string_view()
               : entry.family;
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

/// The facings of a fixture's levels in a fixture of `role`, measured from its manifest as the generator reads it.
std::vector<LevelFacing> facingsOf(const rv::world::PrefabInfo& info, const MediaFamily& family,
                                   rv::world::FixtureRole role) {
    std::vector<LevelFacing> facings;
    for (const rv::world::SlotLevelInfo& level : info.levels) {
        facings.push_back(level.facing(family, role));
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
                                          "case_handheld_v2", "big_box_v2", "generic_case_v2", "arcade_cabinet"});
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
    // systems.json names no family that has a successor in the rooms its systems stand in (I6 step 4: new rooms take
    // the successors): the arcade systems' are the cabinets (I6 step 6), which succeed generic_case in the arcade room.
    for (const rv::world::SystemEntry& entry : systems->systems) {
        INFO(entry.system.str());
        if (entry.kind == rv::world::SystemKind::Arcade) {
            CHECK(entry.family == "arcade_cabinet");
            CHECK(registry().successorOf(entry.family, rv::world::RoomKind::Arcade) == nullptr);
        } else if (entry.kind != rv::world::SystemKind::Excluded) {
            CHECK(registry().successorOf(entry.family, rv::world::RoomKind::Console) == nullptr);
            CHECK(registry().successorOf(entry.family, rv::world::RoomKind::Shared) == nullptr);
        }
    }
    CHECK(registry().successorOf(systems->fallback.family, rv::world::RoomKind::Console) == nullptr);
    CHECK(rv::world::kSharedRoomFamily == "generic_case_v2");
    CHECK(rv::world::kArcadeRoomFamily == "arcade_cabinet");
    CHECK(registry().successorOf(rv::world::kSharedRoomFamily, rv::world::RoomKind::Shared) == nullptr);
    CHECK(registry().successorOf(rv::world::kArcadeRoomFamily, rv::world::RoomKind::Arcade) == nullptr);
    // Every shell material a family names is a material of the library (I6 step 4's review: the cases' dark shells).
    auto materials = rv::assets::MaterialLibrary::loadFromFile(assetRoot() / "materials" / "base" / "materials.json");
    REQUIRE(materials.has_value());
    for (const MediaFamily& family : registry().families) {
        INFO(family.id);
        if (!family.shellMaterial.empty()) {
            CHECK(materials->find(family.shellMaterial) != nullptr);
        }
        // The cases and boxes on their side have dark shells; every other family the theme's plastic.
        CHECK(family.shellMaterial.empty() == (family.spineFacing == rv::world::SpineFacing::End));
    }
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
        // Console and shared rooms migrate to the successor; the arcade room only for the cases other than the generic
        // one (none stands there).
        for (const rv::world::RoomKind kind : {rv::world::RoomKind::Console, rv::world::RoomKind::Shared}) {
            CHECK(registry().successorOf(from, kind) == successor);
            CHECK(registry().latestSuccessor(from, kind) == successor);
            CHECK(registry().latestSuccessor(to, kind) == successor);
        }
        if (from == "generic_case") {
            // The arcade room's generic cases are succeeded by its cabinets (I6 step 6).
            CHECK(registry().successorOf(from, rv::world::RoomKind::Arcade) == registry().find("arcade_cabinet"));
            CHECK(registry().latestSuccessor(from, rv::world::RoomKind::Arcade) == registry().find("arcade_cabinet"));
            CHECK(successor->succeedsIn ==
                  std::vector<rv::world::RoomKind>{rv::world::RoomKind::Console, rv::world::RoomKind::Shared});
        } else {
            CHECK(successor->succeedsIn.empty());
        }
        CHECK(successor->succeeds == from);
        // Its spine label is the flat part of its -X face, top to bottom, the shape its printed spines are drawn at.
        CHECK_FALSE(successor->spineAspect().isLayer());
        CHECK(old->spineAspect().isLayer());
        CHECK_FALSE(successor->shellMaterial.empty());
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
        for (const rv::world::RoomKind kind :
             {rv::world::RoomKind::Console, rv::world::RoomKind::Shared, rv::world::RoomKind::Arcade}) {
            CHECK(registry().successorOf(cartridge, kind) == nullptr);
        }
        CHECK(registry().find(cartridge)->spineAspect().isLayer());
        CHECK(registry().find(cartridge)->spineFacing == rv::world::SpineFacing::End);
        CHECK(registry().find(cartridge)->succeeds.empty());
    }
    CHECK(registry().latestSuccessor("no_such_family", rv::world::RoomKind::Console) == nullptr);

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
    const auto side = [](std::string_view id, std::string_view succeeds, std::string_view slot,
                         std::string_view more = {}) {
        return std::string(R"(, {"id": ")") + std::string(id) + R"(", "succeeds": ")" + std::string(succeeds) +
               R"(", )" + std::string(more) + R"( "spineFacing": "side",
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
    CHECK(good->latestSuccessor("a", rv::world::RoomKind::Console)->id == "b");
    CHECK(good->latestSuccessor("a", rv::world::RoomKind::Arcade)->id == "b");
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
    // Successions scoped by kind of room: two successors of one family in different kinds are sound, in one kind not.
    auto byKind = parse(side("b", "a", kSideSlot, R"("succeedsIn": ["console", "shared"],)") +
                        side("c", "a", kSideSlot, R"("succeedsIn": ["arcade"],)"));
    REQUIRE(byKind.has_value());
    CHECK(byKind->successorOf("a", rv::world::RoomKind::Console)->id == "b");
    CHECK(byKind->successorOf("a", rv::world::RoomKind::Shared)->id == "b");
    CHECK(byKind->successorOf("a", rv::world::RoomKind::Arcade)->id == "c");
    auto overlap = parse(side("b", "a", kSideSlot, R"("succeedsIn": ["console", "shared"],)") +
                         side("c", "a", kSideSlot, R"("succeedsIn": ["shared"],)"));
    REQUIRE_FALSE(overlap.has_value());
    CHECK_THAT(overlap.error().message, ContainsSubstring("has a successor already in shared rooms"));
    auto badKind = parse(side("b", "a", kSideSlot, R"("succeedsIn": ["lobby"],)"));
    REQUIRE_FALSE(badKind.has_value());
    CHECK_THAT(badKind.error().message, ContainsSubstring("console, shared, arcade"));
    auto noKind = parse(side("b", "a", kSideSlot, R"("succeedsIn": [],)"));
    REQUIRE_FALSE(noKind.has_value());
    CHECK_THAT(noKind.error().message, ContainsSubstring("names no kind"));
    // A family facing front (I6 step 6) names every fixture its items stand in; without (or with some) it is refused.
    const auto front = [](std::string_view fixtures) {
        return std::string(R"(, {"id": "f", "spineFacing": "front",
                  "item": {"manifest": "media.screen.arcade.generic", "kind": "cartridge"},
                  "itemWidthMm": 300, "itemHeightMm": 400, "itemDepthMm": 6, "labelWidthMm": 298,
                  "labelHeightMm": 398, "coverFit": "label",
                  "spineOut": {"widthMm": 300, "heightMm": 400, "depthMm": 6, "gapMm": 160, "marginMm": 80,
                               "headroomMm": 0},
                  "faceOut": {"widthMm": 300, "heightMm": 400, "depthMm": 6, "gapMm": 160, "marginMm": 80,
                              "headroomMm": 0})") +
               std::string(fixtures) + "}";
    };
    const std::string cabinet = R"({"manifest": "furniture.arcade_cabinet.generic", "kind": "arcade_cabinet"})";
    auto fronted = parse(front(R"(, "fixtures": {"heroModule": )" + cabinet + R"(, "sideModule": )" + cabinet +
                               R"(, "display": )" + cabinet + R"(, "wallShelf": )" + cabinet + "}"));
    REQUIRE(fronted.has_value());
    CHECK(fronted->find("f")->fixtures.display->kind == "arcade_cabinet");
    for (const std::string& fixtures :
         {std::string(), R"(, "fixtures": {"heroModule": )" + cabinet + R"(, "sideModule": )" + cabinet + "}"}) {
        INFO(fixtures);
        auto bare = parse(front(fixtures));
        REQUIRE_FALSE(bare.has_value());
        CHECK_THAT(bare.error().message, ContainsSubstring("facing front names every fixture"));
    }
    // A cycle: a succeeds c, b succeeds a, c succeeds b (each succeeded once, none by itself) is refused.
    {
        std::string cycle = side("b", "a", kSideSlot) + side("c", "b", kSideSlot);
        auto chain = parse(cycle);
        REQUIRE(chain.has_value());
        CHECK(chain->latestSuccessor("a", rv::world::RoomKind::Console)->id == "c");
        const std::string text = std::string(R"({"schemaVersion": 1, "families": [)") +
                                 R"({"id": "a", "succeeds": "c", "spineFacing": "side",
                                  "item": {"manifest": "media.case.plain_side.generic", "kind": "cartridge"},
                                  "itemWidthMm": 130, "itemHeightMm": 180, "itemDepthMm": 15, "labelWidthMm": 118,
                                  "labelHeightMm": 168, "coverFit": "label", "spineOut": )" +
                                 std::string(kSideSlot) +
                                 R"(, "faceOut": {"widthMm": 130, "heightMm": 180, "depthMm": 15, "gapMm": 4,
                                                "marginMm": 30, "headroomMm": 20}})" +
                                 cycle + "]}";
        auto cyclic = rv::world::parseMediaFamilies(text, "test");
        REQUIRE_FALSE(cyclic.has_value());
        CHECK_THAT(cyclic.error().message, ContainsSubstring("come back to it"));
        // In another kind than the cycle's other links, no cycle.
        const std::string arcadeOnly = std::string(R"({"schemaVersion": 1, "families": [)") +
                                       R"({"id": "a", "succeeds": "c", "succeedsIn": ["arcade"], "spineFacing": "side",
                                        "item": {"manifest": "media.case.plain_side.generic", "kind": "cartridge"},
                                        "itemWidthMm": 130, "itemHeightMm": 180, "itemDepthMm": 15,
                                        "labelWidthMm": 118, "labelHeightMm": 168, "coverFit": "label",
                                        "spineOut": )" +
                                       std::string(kSideSlot) +
                                       R"(, "faceOut": {"widthMm": 130, "heightMm": 180, "depthMm": 15, "gapMm": 4,
                                                      "marginMm": 30, "headroomMm": 20}})" +
                                       side("b", "a", kSideSlot, R"("succeedsIn": ["console"],)") +
                                       side("c", "b", kSideSlot, R"("succeedsIn": ["console"],)") + "]}";
        CHECK(rv::world::parseMediaFamilies(arcadeOnly, "test").has_value());
    }
    // A spine label to a tenth of a millimetre, both sides or neither; a shell material names one.
    CHECK(parse(side("b", "a", kSideSlot, R"("spineLabelWidthMm": 10.7, "spineLabelHeightMm": 177,)"))
              ->find("b")
              ->spineAspect() == rv::assets::LabelAspect{107, 1770});
    auto half = parse(side("b", "a", kSideSlot, R"("spineLabelWidthMm": 10.7,)"));
    REQUIRE_FALSE(half.has_value());
    CHECK_THAT(half.error().message, ContainsSubstring("both its width and its height"));
    auto fine = parse(side("b", "a", kSideSlot, R"("spineLabelWidthMm": 10.75, "spineLabelHeightMm": 177,)"));
    REQUIRE_FALSE(fine.has_value());
    CHECK_THAT(fine.error().message, ContainsSubstring("tenth of a millimetre"));
    auto noShell = parse(side("b", "a", kSideSlot, R"("shellMaterial": "",)"));
    REQUIRE_FALSE(noShell.has_value());

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
    // The arcade room's generic case is succeeded there by its cabinets (I6 step 6): a stored arcade room carries
    // arcade_cabinet's data, a new one takes arcade_cabinet; a stored shared room of generic_case carries
    // generic_case_v2's.
    rv::world::RoomDataRequest arcade;
    arcade.system = systemName("mame");
    arcade.kind = rv::world::RoomKind::Arcade;
    arcade.floor = std::string(rv::world::kGroundFloor);
    auto newArcade = rv::world::loadWorldData(assetRoot(), arcade);
    REQUIRE(newArcade.has_value());
    CHECK(newArcade->mediaFamily.id == "arcade_cabinet");
    CHECK(newArcade->successor == nullptr);
    arcade.mediaFamily = "generic_case";
    auto storedArcade = rv::world::loadWorldData(assetRoot(), arcade);
    REQUIRE(storedArcade.has_value());
    REQUIRE(storedArcade->successor != nullptr);
    CHECK(storedArcade->successor->mediaFamily.id == "arcade_cabinet");
    rv::world::RoomDataRequest shared;
    shared.system = systemName("ps2");
    shared.kind = rv::world::RoomKind::Shared;
    shared.floor = "gen6";
    shared.mediaFamily = "generic_case";
    auto storedShared = rv::world::loadWorldData(assetRoot(), shared);
    REQUIRE(storedShared.has_value());
    REQUIRE(storedShared->successor != nullptr);
    CHECK(storedShared->successor->mediaFamily.id == "generic_case_v2");
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
        if (family.spineFacing == rv::world::SpineFacing::Front) {
            // A cabinet's screen (I6 step 6) never stands spine-out: no spine label.
            CHECK(rv::geometry::findPart(*prefab, "spine") == nullptr);
            CHECK(rv::geometry::findSocket(prefab->sockets, "Spine") == nullptr);
            CHECK(family.spineAspect().isLayer());
            continue;
        }
        CHECK(rv::geometry::findPart(*prefab, "spine") != nullptr);
        const rv::geometry::Socket* spine = rv::geometry::findSocket(prefab->sockets, "Spine");
        REQUIRE(spine != nullptr);
        // The spine label lies on the item's top face, keeping the spine texture's 40 x 256 aspect within 1%; on its
        // -X face, upright, for a family standing on its side, covering that face's flat part top to bottom (I6 step
        // 4's review: a printed spine, drawn at the label's shape, the family's spineAspect()).
        const glm::vec3 spineSize = spine->bounds.max - spine->bounds.min;
        const bool side = family.spineFacing == rv::world::SpineFacing::Side;
        if (side) {
            const auto chamfer = manifest->runtime.params.value("chamfer", 0.0F);
            const auto inset = manifest->runtime.params.value("labelInset", 0.0F);
            const float flatAcross = static_cast<float>(family.itemDepthMm) / 1000.0f - inset - 0.0005f - 2.0f * chamfer;
            const float flatAlong = static_cast<float>(family.itemHeightMm) / 1000.0f - 2.0f * chamfer;
            // The whole flat part, a tenth of a millimetre short where the float limit would refuse it exactly.
            CHECK(spineSize.x == Catch::Approx(flatAcross).margin(1e-5));
            CHECK(spineSize.y <= flatAlong + 1e-6f);
            CHECK(spineSize.y >= flatAlong - 1.1e-4f);
            CHECK(family.spineAspect() ==
                  rv::assets::LabelAspect{static_cast<std::uint32_t>(std::lround(spineSize.x * 10000.0f)),
                                          static_cast<std::uint32_t>(std::lround(spineSize.y * 10000.0f))});
        } else {
            CHECK(spineSize.x / spineSize.y > 0.99f * 40.0f / 256.0f);
            CHECK(spineSize.x / spineSize.y < 1.01f * 40.0f / 256.0f);
            CHECK(family.spineAspect().isLayer());
        }
        CHECK(spineSize.x * 1000.0f <= static_cast<float>(family.itemDepthMm));
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
    // A spine label that is not the manifest's is refused.
    MediaFamily shorter = *registry().find("disc_case_cd_v2");
    shorter.spineLabel.height = 1200;
    auto sideManifest =
        rv::assets::loadPrefabManifest(rv::assets::prefabManifestPath(assetRoot(), shorter.item.manifest));
    REQUIRE(sideManifest.has_value());
    auto wrongSpine = rv::world::checkItemManifest(shorter, *sideManifest);
    REQUIRE_FALSE(wrongSpine.has_value());
    CHECK_THAT(wrongSpine.error().message, ContainsSubstring("spine label is 71 x 1234"));
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
        const bool front = family.spineFacing == rv::world::SpineFacing::Front;
        // The side-wall module: every level spine-out (a cabinet's screen as the spine section's, I6 step 6).
        const rv::world::PrefabInfo side = measure(roomTemplate.sideModule);
        REQUIRE_FALSE(side.levels.empty());
        for (const LevelFacing facing : facingsOf(side, family, rv::world::FixtureRole::SideModule)) {
            CHECK(facing == LevelFacing::SpineOut);
        }
        // The hero-wall module: one face-out band, the other levels spine-out (the cabinets: no band).
        const rv::world::PrefabInfo hero = measure(roomTemplate.heroModule);
        std::size_t band = 0;
        for (const LevelFacing facing : facingsOf(hero, family, rv::world::FixtureRole::HeroModule)) {
            CHECK(facing != LevelFacing::Other);
            band += facing == LevelFacing::FaceOut ? 1 : 0;
        }
        CHECK(band == (front ? 0u : 1u));
        CHECK(hero.slots(family, LevelFacing::SpineOut, rv::world::FixtureRole::HeroModule) > 0);
        // The display fixtures: every level face-out.
        std::set<std::string> kinds;
        for (const std::string& socketId : roomTemplate.displayOrder) {
            const rv::world::TemplateSocket* socket = roomTemplate.socket(socketId);
            REQUIRE(socket != nullptr);
            REQUIRE(socket->prefab.has_value());
            kinds.insert(socket->prefab->kind);
            const rv::world::PrefabInfo display = measure(*socket->prefab);
            REQUIRE_FALSE(display.levels.empty());
            for (const LevelFacing facing : facingsOf(display, family, rv::world::FixtureRole::Display)) {
                CHECK(facing == LevelFacing::FaceOut);
            }
            // Within its socket.
            CHECK(display.widthMm() <= socket->widthMm);
            CHECK(display.depthMm() <= socket->depthMm);
        }
        CHECK(kinds == (front ? std::set<std::string>{"arcade_cabinet"}
                              : std::set<std::string>{"display_case", "wall_shelf"}));
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
            REQUIRE_FALSE(plan.rooms.empty());
            // A room alone, or a chain past the template's limit (generator 1.5): every room of the family.
            for (const rv::world::Room& room : plan.rooms) {
                CHECK(room.mediaFamily == entry.family);
            }
            // Every title placed or unplaced for capacity, which only a full floor leaves (a chain's rooms hold the
            // rest: the arcade room's 2,600 cabinets need 50 rooms, its floor takes 24); the fixtures the family's own.
            std::size_t capacity = 0;
            for (const rv::world::UnplacedGame& game : plan.unplaced) {
                capacity += game.reason == rv::world::UnplacedReason::Capacity ? 1 : 0;
            }
            CHECK(plan.placements.size() + capacity == titles);
            if (capacity > 0) {
                CHECK(plan.rooms.size() == 2 * data.building.layout.maxRoomsPerRow);
            }
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

TEST_CASE("world/families: cases on their side fill the hero-wall modules in turns, cartridges one after the other",
          "[world][families]") {
    // Generator 1.4's rule for a family standing on its side (allocation.hpp, I6 step 4's review): the hero-wall
    // modules take their levels in turns, the titles a round cannot fill shared among its levels, so that a small room
    // shows its spines on both modules at the eye; a family on its end (the cartridges) fills module after module.
    struct Case {
        std::string_view system;
        std::string_view family;
        bool turns;
    };
    for (const Case& test : {Case{"psx", "disc_case_cd_v2", true}, Case{"amiga", "big_box_v2", true},
                             Case{"ps2", "disc_case_dvd_v2", true}, Case{"snes", "cartridge_home_16bit", false}}) {
        INFO(test.system);
        const WorldData data = dataOf(test.system);
        REQUIRE(data.mediaFamily.id == test.family);
        for (const std::size_t spineTitles : {std::size_t{1}, std::size_t{9}, std::size_t{40}}) {
            INFO(spineTitles);
            // The face-out set first (D + B), then the spine-out titles.
            auto capacity = rv::world::collectionCapacity(data.roomTemplate, data.prefabs, data.mediaFamily);
            REQUIRE(capacity.has_value());
            const std::size_t faceOut = capacity->displaySlots + capacity->bandSlots;
            rv::world::GenerationInput input;
            input.library = rv::test::world::library();
            input.config = rv::test::world::roomConfig(102938);
            input.selection = selectionOf(test.system, faceOut + spineTitles, 4100 + spineTitles);
            auto result = rv::world::generate(input, data);
            REQUIRE(result.has_value());
            CHECK(result->validation.passed());
            const rv::world::WorldPlan& plan = result->plan;
            // The spine-out titles of each hero-wall module, by its levels.
            std::map<rv::world::FixtureId, std::map<std::uint32_t, std::size_t>> spineByModule;
            std::map<rv::world::FixtureId, const rv::world::Fixture*> heroModules;
            for (const rv::world::Fixture& fixture : plan.fixtures) {
                if (fixture.role == rv::world::FixtureRole::HeroModule) {
                    heroModules.emplace(fixture.id, &fixture);
                    spineByModule[fixture.id];
                }
            }
            REQUIRE(heroModules.size() == 2);
            std::size_t spinePlaced = 0;
            for (const rv::world::Placement& placement : plan.placements) {
                const auto module = heroModules.find(placement.slot.fixture());
                if (module == heroModules.end()) {
                    continue;
                }
                for (const rv::world::FixtureLevel& level : module->second->levels) {
                    if (level.level == placement.slot.level() && level.section == rv::world::Section::Spine) {
                        ++spineByModule[module->first][level.level];
                        ++spinePlaced;
                    }
                }
            }
            CHECK(spinePlaced == spineTitles);
            std::vector<std::size_t> perModule;
            std::vector<std::size_t> levelsUsed;
            for (const auto& [module, levels] : spineByModule) {
                std::size_t total = 0;
                for (const auto& [level, count] : levels) {
                    total += count;
                }
                perModule.push_back(total);
                levelsUsed.push_back(levels.size());
            }
            // In the modules' map order (by id), not their fill order: the larger share first.
            if (perModule[0] < perModule[1]) {
                std::swap(perModule[0], perModule[1]);
                std::swap(levelsUsed[0], levelsUsed[1]);
            }
            if (test.turns) {
                // Shared between the two modules (the hero-wall modules' levels identical, each round's split evenly),
                // one more on the first for an odd count; a round holding them all, one level each.
                CHECK(perModule[0] + perModule[1] == spineTitles);
                CHECK(perModule[0] - perModule[1] <= 1);
                if (spineTitles <= 9) {
                    CHECK(levelsUsed[0] == 1);
                    CHECK(levelsUsed[1] == (spineTitles > 1 ? 1u : 0u));
                } else {
                    CHECK(levelsUsed[0] - levelsUsed[1] <= 1);
                }
            } else {
                // The first module first: the second holds none while the first has room.
                CHECK(perModule[0] == spineTitles);
                CHECK(perModule[1] == 0);
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
        {"mame", {}, 6, 0, 4, 2, 33, 53},                       // arcade_cabinet (I6 step 6): a cabinet per slot
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

TEST_CASE("world/families: the arcade room's cabinets face out in every fixture (I6 step 6)", "[world][families][arcade]") {
    const MediaFamily* cabinets = registry().find("arcade_cabinet");
    REQUIRE(cabinets != nullptr);
    CHECK(cabinets->spineFacing == rv::world::SpineFacing::Front);
    CHECK(cabinets->spineOut.widthMm == cabinets->faceOut.widthMm);
    CHECK(cabinets->spineOut.heightMm == cabinets->faceOut.heightMm);
    CHECK(cabinets->spineOut.depthMm == cabinets->faceOut.depthMm);
    CHECK(cabinets->succeeds == "generic_case");
    CHECK(cabinets->succeedsIn == std::vector<rv::world::RoomKind>{rv::world::RoomKind::Arcade});
    CHECK(cabinets->labelMaterial == "MAT_Screen_Arcade");
    CHECK(cabinets->fixtures.heroModule->kind == "arcade_cabinet");
    CHECK(cabinets->fixtures.display == cabinets->fixtures.sideModule);
    CHECK(rv::world::kArcadeRoomFamily == "arcade_cabinet");
    // Its materials exist in the library.
    auto library = rv::assets::MaterialLibrary::loadFromFile(assetRoot() / "materials" / "base" / "materials.json");
    REQUIRE(library.has_value());
    CHECK(library->find(cabinets->labelMaterial) != nullptr);
    CHECK(library->find(cabinets->shellMaterial) != nullptr);

    // The template as a room of cabinets stands it: no shelf dressing, the cabinets in every collection socket.
    const rv::world::RoomTemplate stood = rv::world::withFamilyFixtures(rv::test::world::snesData().roomTemplate,
                                                                         *cabinets);
    CHECK(stood.shelfDressing.empty());
    CHECK(stood.sideModule.kind == "arcade_cabinet");

    // A level's facing by its fixture's role: the modules' the spine section's, the display fixtures' face-out; the
    // volume alone says face-out.
    rv::world::SlotLevelInfo screen;
    screen.level = 0;
    screen.slots = 2;
    screen.slotWidthMm = 300;
    screen.slotHeightMm = 400;
    screen.slotDepthMm = 6;
    CHECK(screen.facing(*cabinets) == LevelFacing::FaceOut);
    CHECK(screen.facing(*cabinets, rv::world::FixtureRole::SideModule) == LevelFacing::SpineOut);
    CHECK(screen.facing(*cabinets, rv::world::FixtureRole::HeroModule) == LevelFacing::SpineOut);
    CHECK(screen.facing(*cabinets, rv::world::FixtureRole::Display) == LevelFacing::FaceOut);
    // Another family's face-out level stays face-out in a module (its hero band).
    const MediaFamily* sixteen = registry().find("cartridge_home_16bit");
    rv::world::SlotLevelInfo band;
    band.slotWidthMm = 125;
    band.slotHeightMm = 85;
    band.slotDepthMm = 20;
    CHECK(band.facing(*sixteen, rv::world::FixtureRole::HeroModule) == LevelFacing::FaceOut);

    // The arcade room of the real library's size (38 titles): every title in a cabinet, the featured six in the
    // display pairs, the rest in the fill order, the room grown a step; no dressing.
    const WorldData data = dataOf("mame");
    REQUIRE(data.mediaFamily.id == "arcade_cabinet");
    rv::world::GenerationInput input;
    input.library = rv::test::world::library();
    input.selection = selectionOf("mame", 38, 4600);
    input.config = rv::test::world::roomConfig(102938);
    auto result = rv::world::generate(input, data);
    REQUIRE(result.has_value());
    CHECK(result->validation.passed());
    const rv::world::WorldPlan& plan = result->plan;
    CHECK(plan.placements.size() == 38);
    CHECK(plan.dressing.empty());
    CHECK(plan.rooms.front().growthSteps == 1);
    std::size_t featured = 0;
    for (const rv::world::Fixture& fixture : plan.fixtures) {
        if (fixture.role == rv::world::FixtureRole::HeroModule || fixture.role == rv::world::FixtureRole::SideModule ||
            fixture.role == rv::world::FixtureRole::Display) {
            CHECK(fixture.prefab.kind == "arcade_cabinet");
            for (const rv::world::FixtureLevel& level : fixture.levels) {
                CHECK(level.section == (fixture.role == rv::world::FixtureRole::Display ? rv::world::Section::Featured
                                                                                         : rv::world::Section::Spine));
                featured += level.section == rv::world::Section::Featured ? level.slots : 0;
            }
        }
    }
    CHECK(featured == 6);
    // The featured are the first six by priority.
    std::set<rv::catalog::GameId> first;
    for (std::size_t i = 0; i < 6; ++i) {
        first.insert(input.selection.titles[i].game);
    }
    std::set<rv::catalog::GameId> shown;
    for (const rv::world::Placement& placement : plan.placements) {
        for (const rv::world::Fixture& fixture : plan.fixtures) {
            if (fixture.id == placement.slot.fixture() && fixture.role == rv::world::FixtureRole::Display) {
                shown.insert(placement.game);
            }
        }
    }
    CHECK(shown == first);

    // An arcade over its capacity (53 titles at 16 m, ADR-0014 §6 amended): since generator 1.5 (I4b step 7, ADR-0015
    // §7) 60 titles stand in a chain of two rooms of 30, area:arcade and area:arcade:2, split in alphabetical order,
    // each with its own featured titles, the first six of its share by priority; the same twice.
    rv::world::GenerationInput over = input;
    over.selection = selectionOf("mame", 60, 4601);
    auto large = rv::world::generate(over, data);
    REQUIRE(large.has_value());
    CHECK(large->validation.passed());
    CHECK(large->plan.placements.size() == 60);
    REQUIRE(large->plan.rooms.size() == 2);
    CHECK(large->plan.rooms[1].id == *large->plan.rooms[0].id.inChain(2));
    CHECK(large->plan.rooms[1].expands == large->plan.rooms[0].id);
    std::map<rv::world::RoomId, std::vector<rv::world::RoomTitle>> shares;
    std::map<rv::catalog::GameId, rv::world::RoomId> roomOfGame;
    for (const rv::world::Placement& placement : large->plan.placements) {
        roomOfGame.emplace(placement.game, placement.slot.fixture().room());
    }
    for (const rv::world::RoomTitle& title : over.selection.titles) {
        shares[roomOfGame.at(title.game)].push_back(title); // in priority order
    }
    REQUIRE(shares.size() == 2);
    for (const auto& [room, titles] : shares) {
        INFO(room.toString());
        CHECK(titles.size() == 30);
        std::set<rv::catalog::GameId> firstSix;
        for (std::size_t i = 0; i < 6; ++i) {
            firstSix.insert(titles[i].game);
        }
        std::set<rv::catalog::GameId> featuredHere;
        for (const rv::world::Placement& placement : large->plan.placements) {
            for (const rv::world::Fixture& fixture : large->plan.fixtures) {
                if (fixture.id == placement.slot.fixture() && fixture.id.room() == room &&
                    fixture.role == rv::world::FixtureRole::Display) {
                    featuredHere.insert(placement.game);
                }
            }
        }
        CHECK(featuredHere == firstSix);
    }
    // The head's share comes first in the alphabet.
    std::string lastOfHead;
    std::string firstOfExpansion = "~";
    for (const rv::world::RoomTitle& title : over.selection.titles) {
        if (roomOfGame.at(title.game) == large->plan.rooms[0].id) {
            lastOfHead = std::max(lastOfHead, title.sortKey);
        } else {
            firstOfExpansion = std::min(firstOfExpansion, title.sortKey);
        }
    }
    CHECK(lastOfHead <= firstOfExpansion);
    CHECK(std::none_of(large->plan.unplaced.begin(), large->plan.unplaced.end(), [](const rv::world::UnplacedGame& g) {
        return g.reason == rv::world::UnplacedReason::Capacity;
    }));
    auto again = rv::world::generate(over, data);
    REQUIRE(again.has_value());
    CHECK(again->plan == large->plan);
}
