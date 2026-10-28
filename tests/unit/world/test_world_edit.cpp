// The edits of a plan and its overrides (I4b step 2, ADR-0015 §1, §2, §4): each operation and each refusal by rule, the
// world untouched by a refusal; every manual change pinned; the overrides in the inputDigest (a world without overrides
// keeping its digest, an edit agreeing with a generation that honours the same pins, a pin undone restoring the plan
// byte for byte); an edit then an update of the same selection writing nothing; an update with additions keeping every
// pin and moved fixture (§21.3), the sockets a moved fixture overlaps skipped and a moved fixture never moved by
// growth; random edit sessions (property); the edit script's format.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/random.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_edit.hpp>

#include "world_building_support.hpp"
#include "world_generation_support.hpp"
#include "world_update_support.hpp"

namespace {

using rv::catalog::GameId;
using rv::world::EditData;
using rv::world::EditedWorld;
using rv::world::EditKind;
using rv::world::EditOperation;
using rv::world::EditResult;
using rv::world::Fixture;
using rv::world::FixtureId;
using rv::world::FixtureRole;
using rv::world::GenerationResult;
using rv::world::Placement;
using rv::world::RoomSelection;
using rv::world::SlotId;
using rv::world::WorldPlan;
using rv::test::world::generateSelection;
using rv::test::world::makeSelection;
using rv::test::world::snesData;
using rv::test::world::updateRoom;

constexpr std::uint64_t kWorldSeed = 102938;
const rv::world::PinProvenance kProvenance{rv::world::PinOrigin::Editor, "2026-10-27T10:00:00.000Z"};

std::shared_ptr<const rv::world::WorldData> unowned(const rv::world::WorldData& data) {
    return std::shared_ptr<const rv::world::WorldData>(std::shared_ptr<const rv::world::WorldData>(), &data);
}

/// The edit data of a plan whose rooms all stand on the shipped data of the SNES room's floor.
EditData snesEditData(std::uint64_t worldSeed = kWorldSeed) {
    EditData data;
    data.config = rv::test::world::roomConfig(worldSeed);
    data.building = snesData().building;
    data.rooms.emplace(*rv::world::RoomId::forSystem(rv::test::world::snes()), unowned(snesData()));
    return data;
}

EditResult edit(EditedWorld& world, const EditOperation& operation, const EditData& data) {
    auto result = rv::world::applyEdit(world, operation, data, kProvenance);
    INFO((result ? std::string("applied") : result.error().toString()));
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// The edit refused by `rule`, the world unchanged.
void requireRefused(EditedWorld& world, const EditOperation& operation, const EditData& data, std::string_view rule) {
    const EditedWorld before = world;
    const EditResult result = edit(world, operation, data);
    INFO(rv::world::editText(operation));
    REQUIRE(result.refused.has_value());
    INFO(result.refused->rule << " " << result.refused->subject << ": " << result.refused->message);
    CHECK(result.refused->rule == rule);
    CHECK_FALSE(result.changed);
    CHECK(world == before);
}

const Placement* placementOf(const WorldPlan& plan, const GameId& game) {
    for (const Placement& placement : plan.placements) {
        if (placement.game == game) {
            return &placement;
        }
    }
    return nullptr;
}

const Fixture* fixtureOf(const WorldPlan& plan, std::string_view id) {
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.toString() == id) {
            return &fixture;
        }
    }
    return nullptr;
}

/// The plan's slots of `section` that hold no game, dressed or not.
std::vector<SlotId> freeSlots(const WorldPlan& plan, rv::world::Section section, bool dressed) {
    std::set<SlotId> taken;
    for (const Placement& placement : plan.placements) {
        taken.insert(placement.slot);
    }
    std::vector<SlotId> slots;
    for (const Fixture& fixture : plan.fixtures) {
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            if (level.section != section) {
                continue;
            }
            for (std::uint32_t index = 0; index < level.slots; ++index) {
                const SlotId slot = *SlotId::make(fixture.id, level.level, index);
                const bool covered = std::any_of(plan.dressing.begin(), plan.dressing.end(), [&](const auto& span) {
                    return span.level == slot.levelId() && index >= span.firstSlot &&
                           index < span.firstSlot + span.count;
                });
                if (!taken.contains(slot) && covered == dressed) {
                    slots.push_back(slot);
                }
            }
        }
    }
    return slots;
}

/// The placements of `section`.
std::vector<Placement> placementsIn(const WorldPlan& plan, rv::world::Section section) {
    const auto sections = rv::test::world::levelSections(plan);
    std::vector<Placement> found;
    for (const Placement& placement : plan.placements) {
        const auto level = sections.find({placement.slot.fixture(), placement.slot.level()});
        if (level != sections.end() && level->second == section) {
            found.push_back(placement);
        }
    }
    return found;
}

/// Every pin of `world` honoured by its plan, none orphaned.
void requirePinsHonoured(const EditedWorld& world) {
    const rv::world::PinCheck check = rv::world::checkPins(world.plan, {snesData().roomTemplate}, world.overrides);
    INFO((check.unhonoured.empty() ? std::string() : check.unhonoured.front()));
    CHECK(check.unhonoured.empty());
    CHECK(check.orphaned.empty());
    CHECK(check.honoured == world.overrides);
}

/// The world of a fresh SNES room of `titles` titles (selection seed `seed`).
struct Fresh {
    RoomSelection selection;
    GenerationResult generated;
    EditedWorld world;
};
Fresh freshRoom(std::size_t titles, std::uint64_t seed) {
    Fresh fresh;
    fresh.selection = makeSelection(titles, seed);
    fresh.generated = generateSelection(fresh.selection, kWorldSeed);
    REQUIRE(fresh.generated.validation.passed());
    fresh.world = EditedWorld{fresh.generated.plan, {}};
    return fresh;
}

/// The update of `world` to `selection` with its overrides.
GenerationResult updated(const EditedWorld& world, const RoomSelection& selection) {
    return updateRoom(world.plan, selection, kWorldSeed, world.overrides);
}

/// The first fixture of `plan` of `role` that `accept` takes, or null.
template <class Accept>
const Fixture* findFixture(const WorldPlan& plan, FixtureRole role, Accept&& accept) {
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.role == role && accept(fixture)) {
            return &fixture;
        }
    }
    return nullptr;
}

const rv::world::TemplateSocket* socketOf(const Fixture& fixture) {
    return snesData().roomTemplate.socket(fixture.id.socket());
}

} // namespace

TEST_CASE("world/edit: a game moved into a free slot is pinned there, and the update of the same selection writes "
          "nothing",
          "[world][edit]") {
    Fresh fresh = freshRoom(80, 57);
    const EditData data = snesEditData();
    const std::vector<Placement> spine = placementsIn(fresh.world.plan, rv::world::Section::Spine);
    const std::vector<SlotId> free = freeSlots(fresh.world.plan, rv::world::Section::Spine, false);
    REQUIRE(spine.size() > 2);
    REQUIRE_FALSE(free.empty());
    const Placement moved = spine.front();
    const SlotId to = free.back();

    const EditResult result = edit(fresh.world, EditOperation::move(moved.game, to), data);
    REQUIRE_FALSE(result.refused);
    CHECK(result.changed);
    CHECK(result.rebuild == rv::world::RebuildLevel::NoRebuild);
    CHECK(result.validation.passed());
    CHECK(result.floor == fresh.world.plan.rooms.front().floor);
    REQUIRE(placementOf(fresh.world.plan, moved.game) != nullptr);
    CHECK(placementOf(fresh.world.plan, moved.game)->slot == to);
    REQUIRE(fresh.world.overrides.gamePins.size() == 1);
    CHECK(fresh.world.overrides.gamePins.front() == rv::world::GamePin{moved.game, to, kProvenance});
    CHECK(fresh.world.overrides.fixturePins.empty());
    // The overrides joined the inputDigest; the plan validates as a whole with its pins.
    CHECK(fresh.world.plan.inputDigest != fresh.generated.plan.inputDigest);
    CHECK(rv::world::isSha256Hex(fresh.world.plan.inputDigest));
    requirePinsHonoured(fresh.world);

    // An update of the same selection keeps it all and writes nothing (the plan byte for byte, its digest included).
    const GenerationResult update = updated(fresh.world, fresh.selection);
    CHECK(update.validation.passed());
    CHECK(update.plan == fresh.world.plan);
    CHECK(update.changes.digestAfter == update.changes.digestBefore);
    CHECK(update.changes.needsConfirmation.empty());
    CHECK(update.changes.orphanedPins.empty());

    // Moved back and unpinned: the plan as generated, byte for byte (its offset drawn again alike, the digest's term
    // taken out again).
    const EditResult back = edit(fresh.world, EditOperation::move(moved.game, moved.slot), data);
    REQUIRE_FALSE(back.refused);
    CHECK(fresh.world.overrides.gamePins.front().slot == moved.slot);
    const EditResult unpinned = edit(fresh.world, EditOperation::unpin(moved.game), data);
    CHECK(unpinned.changed);
    CHECK(fresh.world.overrides.empty());
    CHECK(fresh.world.plan == fresh.generated.plan);
    CHECK(rv::world::planDigest(fresh.world.plan).value() == fresh.generated.changes.digestAfter);
}

TEST_CASE("world/edit: face-out slots: the dressing gives way where a game stands, a face-out slot left is dressed",
          "[world][edit]") {
    // A small room: its titles all on the display fixtures, every slot they leave dressed.
    Fresh fresh = freshRoom(6, 11);
    const EditData data = snesEditData();
    const std::vector<SlotId> dressed = freeSlots(fresh.world.plan, rv::world::Section::Featured, true);
    REQUIRE_FALSE(dressed.empty());
    REQUIRE(placementsIn(fresh.world.plan, rv::world::Section::Spine).empty());
    const Placement featured = placementsIn(fresh.world.plan, rv::world::Section::Featured).front();
    const SlotId onDisplay = dressed.back();
    const std::size_t spans = fresh.world.plan.dressing.size();

    // Onto a dressed slot of a display: the span split around it; the face-out slot it left dressed with one slot.
    const EditResult onto = edit(fresh.world, EditOperation::move(featured.game, onDisplay), data);
    REQUIRE_FALSE(onto.refused);
    CHECK(onto.rebuild == rv::world::RebuildLevel::LocalReflow);
    CHECK(onto.validation.passed());
    const std::vector<SlotId> stillDressed = freeSlots(fresh.world.plan, rv::world::Section::Featured, true);
    CHECK(std::find(stillDressed.begin(), stillDressed.end(), onDisplay) == stillDressed.end());
    CHECK(std::find(stillDressed.begin(), stillDressed.end(), featured.slot) != stillDressed.end());
    CHECK(stillDressed.size() == dressed.size());
    const auto left = std::find_if(fresh.world.plan.dressing.begin(), fresh.world.plan.dressing.end(),
                                   [&](const rv::world::DressedLevel& span) {
                                       return span.level == featured.slot.levelId() &&
                                              span.firstSlot == featured.slot.index() && span.count == 1;
                                   });
    CHECK(left != fresh.world.plan.dressing.end());
    CHECK(fresh.world.plan.dressing.size() >= spans);
    // Face-out: turned, not pushed in.
    const Placement* standing = placementOf(fresh.world.plan, featured.game);
    REQUIRE(standing != nullptr);
    CHECK(standing->insetMm == 0);

    // Onto the spine-out shelves, which are dressed while no title needs them: pushed in, not turned.
    const SlotId spineSlot = freeSlots(fresh.world.plan, rv::world::Section::Spine, true).front();
    const EditResult off = edit(fresh.world, EditOperation::move(featured.game, spineSlot), data);
    REQUIRE_FALSE(off.refused);
    CHECK(off.rebuild == rv::world::RebuildLevel::LocalReflow);
    CHECK(placementOf(fresh.world.plan, featured.game)->yawCdeg == 0);
    const std::vector<SlotId> spineDressed = freeSlots(fresh.world.plan, rv::world::Section::Spine, true);
    CHECK(std::find(spineDressed.begin(), spineDressed.end(), spineSlot) == spineDressed.end());
    CHECK(fresh.world.overrides.gamePins.size() == 1);
    requirePinsHonoured(fresh.world);
    const GenerationResult update = updated(fresh.world, fresh.selection);
    CHECK(update.plan == fresh.world.plan);
}

TEST_CASE("world/edit: games swapped, pinned and unpinned; refusals by rule leave the world as it was",
          "[world][edit]") {
    Fresh fresh = freshRoom(80, 23);
    const EditData data = snesEditData();
    const std::vector<Placement> spine = placementsIn(fresh.world.plan, rv::world::Section::Spine);
    const Placement band = placementsIn(fresh.world.plan, rv::world::Section::Band).front();
    const Placement a = spine.at(0);
    const Placement b = spine.at(1);

    SECTION("swap") {
        const EditResult swapped = edit(fresh.world, EditOperation::swap(a.game, band.game), data);
        REQUIRE_FALSE(swapped.refused);
        CHECK(placementOf(fresh.world.plan, a.game)->slot == band.slot);
        CHECK(placementOf(fresh.world.plan, band.game)->slot == a.slot);
        CHECK(fresh.world.overrides.gamePins.size() == 2);
        requirePinsHonoured(fresh.world);
        CHECK(updated(fresh.world, fresh.selection).plan == fresh.world.plan);
        requireRefused(fresh.world, EditOperation::swap(a.game, a.game), data, rv::world::edit_rule::kSameGame);
    }
    SECTION("pin and unpin in place") {
        const EditResult pinned = edit(fresh.world, EditOperation::pin(a.game), data);
        CHECK(pinned.changed);
        CHECK(pinned.rooms.empty());
        CHECK(fresh.world.plan.placements == fresh.generated.plan.placements);
        CHECK(fresh.world.plan.inputDigest != fresh.generated.plan.inputDigest);
        const EditResult again = edit(fresh.world, EditOperation::pin(a.game), data);
        CHECK_FALSE(again.changed);
        CHECK_FALSE(again.refused);
        CHECK(updated(fresh.world, fresh.selection).plan == fresh.world.plan);
        CHECK(edit(fresh.world, EditOperation::unpin(a.game), data).changed);
        CHECK_FALSE(edit(fresh.world, EditOperation::unpin(a.game), data).changed);
        CHECK(fresh.world.plan == fresh.generated.plan);
        // An orphaned pin (a game the plan does not hold) is unpinned too.
        const GameId stranger = *GameId::fromValue(0x1234);
        fresh.world.overrides.gamePins.push_back({stranger, a.slot, {}});
        CHECK(edit(fresh.world, EditOperation::unpin(stranger), data).changed);
        CHECK(fresh.world.overrides.empty());
    }
    SECTION("refusals") {
        // Onto another game: swap instead.
        requireRefused(fresh.world, EditOperation::move(a.game, b.slot), data, rv::world::edit_rule::kSlotTaken);
        // A slot the plan lacks.
        requireRefused(fresh.world, EditOperation::move(a.game, *SlotId::make(a.slot.fixture(), a.slot.level(), 9999)),
                       data, rv::world::rule::kSlotUnknown);
        // An unplaced game is never placed by an edit, nor pinned.
        const rv::world::UnplacedGame unplaced = fresh.generated.plan.unplaced.front();
        const SlotId free = freeSlots(fresh.world.plan, rv::world::Section::Spine, false).front();
        requireRefused(fresh.world, EditOperation::move(unplaced.game, free), data, rv::world::edit_rule::kGameUnknown);
        requireRefused(fresh.world, EditOperation::pin(unplaced.game), data, rv::world::edit_rule::kGameUnknown);
        requireRefused(fresh.world, EditOperation::swap(a.game, unplaced.game), data,
                       rv::world::edit_rule::kGameUnknown);
        // A slot pinned to another game (written by hand) takes no other.
        fresh.world.overrides.gamePins.push_back({b.game, free, {}});
        requireRefused(fresh.world, EditOperation::move(a.game, free), data, rv::world::edit_rule::kSlotPinned);
    }
}

TEST_CASE("world/edit: games move within their room chain only", "[world][edit]") {
    rv::test::world::BuildingWorld world = rv::test::world::shippedWorld(kWorldSeed);
    world.config.sharedRoomMinTitles = 0;
    const RoomSelection snes = rv::test::world::systemSelection("snes", 40, 5);
    const RoomSelection megadrive = rv::test::world::systemSelection("megadrive", 40, 6);
    const GenerationResult building = rv::test::world::generateBuildingOf(world, {snes, megadrive});
    REQUIRE(building.validation.passed());
    EditData data;
    data.config = world.config;
    data.building = world.building;
    for (const rv::world::Room& room : building.plan.rooms) {
        data.rooms.emplace(room.id, unowned(rv::test::world::storedData(world.root, room)));
    }
    EditedWorld edited{building.plan, {}};
    const Placement* fromSnes = nullptr;
    const Placement* fromMegadrive = nullptr;
    for (const Placement& placement : edited.plan.placements) {
        const std::string room = placement.slot.fixture().room().toString();
        fromSnes = fromSnes == nullptr && room == "room:snes" ? &placement : fromSnes;
        fromMegadrive = fromMegadrive == nullptr && room == "room:megadrive" ? &placement : fromMegadrive;
    }
    REQUIRE(fromSnes != nullptr);
    REQUIRE(fromMegadrive != nullptr);
    const Placement snesGame = *fromSnes;
    const Placement megadriveGame = *fromMegadrive;
    std::optional<SlotId> freeInMegadrive;
    for (const bool dressed : {false, true}) {
        for (const SlotId& slot : freeSlots(edited.plan, rv::world::Section::Spine, dressed)) {
            if (!freeInMegadrive && slot.fixture().room() == megadriveGame.slot.fixture().room()) {
                freeInMegadrive = slot;
            }
        }
    }
    INFO("megadrive placements " << std::count_if(edited.plan.placements.begin(), edited.plan.placements.end(),
                                                  [&](const Placement& p) {
                                                      return p.slot.fixture().room() == megadriveGame.slot.fixture().room();
                                                  })
                                 << ", spine free " << freeSlots(edited.plan, rv::world::Section::Spine, false).size()
                                 << ", room " << megadriveGame.slot.toString());
    REQUIRE(freeInMegadrive.has_value());
    requireRefused(edited, EditOperation::move(snesGame.game, *freeInMegadrive), data,
                   rv::world::edit_rule::kOtherRoom);
    requireRefused(edited, EditOperation::swap(snesGame.game, megadriveGame.game), data,
                   rv::world::edit_rule::kOtherRoom);
    // Within its own room it moves, and editRooms() names the floor's rooms the session needs.
    const EditResult moved = edit(edited, EditOperation::move(megadriveGame.game, *freeInMegadrive), data);
    REQUIRE_FALSE(moved.refused);
    const std::vector<EditOperation> session{EditOperation::move(megadriveGame.game, *freeInMegadrive)};
    for (const rv::world::RoomId& room : rv::world::editRooms(edited.plan, session)) {
        CHECK(data.rooms.contains(room));
    }
    // Data missing for a room of the floor: an error, the world as it was.
    EditData lacking = data;
    lacking.rooms.erase(megadriveGame.slot.fixture().room());
    const EditedWorld before = edited;
    CHECK_FALSE(rv::world::applyEdit(edited, EditOperation::move(megadriveGame.game, megadriveGame.slot), lacking,
                                     kProvenance)
                    .has_value());
    CHECK(edited == before);
}

TEST_CASE("world/edit: the overrides join the inputDigest; a world without overrides keeps its digest",
          "[world][edit]") {
    const RoomSelection selection = makeSelection(50, 31);
    rv::world::GenerationInput input;
    input.library = rv::test::world::library();
    input.selection = selection;
    input.config = rv::test::world::roomConfig(kWorldSeed);
    const GenerationResult plain = *rv::world::generate(input, snesData());
    // No override: the digest is the inputs' alone, as before I4b.
    CHECK(plain.plan.inputDigest == rv::world::generationInputDigest(input, snesData()).value());

    // A pin the edit makes and the same pin given to a generation that honours it: one digest.
    const Placement placed = plain.plan.placements.front();
    EditedWorld world{plain.plan, {}};
    REQUIRE(edit(world, EditOperation::pin(placed.game), snesEditData()).changed);
    input.overrides = world.overrides;
    const GenerationResult pinned = *rv::world::generate(input, snesData());
    CHECK(pinned.plan.inputDigest == world.plan.inputDigest);
    CHECK(pinned.plan == world.plan);
    // Provenance is left out of the digest.
    input.overrides.gamePins.front().provenance = {rv::world::PinOrigin::Cli, ""};
    CHECK(rv::world::generate(input, snesData())->plan.inputDigest == world.plan.inputDigest);
    // A pin the plan does not honour (orphaned) is left out too.
    input.overrides.gamePins.front().game = *GameId::fromValue(0x77);
    CHECK(rv::world::generate(input, snesData())->plan.inputDigest == plain.plan.inputDigest);
}

TEST_CASE("world/edit: fixtures moved on the grid and turned, pinned where put; refusals by rule", "[world][edit]") {
    Fresh fresh = freshRoom(40, 3);
    const EditData data = snesEditData();
    const WorldPlan& plan = fresh.world.plan;
    const rv::world::Room& room = plan.rooms.front();
    // A decoration standing on the floor of a side wall, another hung on one, the display cases.
    const Fixture* standing = findFixture(plan, FixtureRole::Decoration, [](const Fixture& fixture) {
        const auto* socket = socketOf(fixture);
        return fixture.yMm == 0 && socket != nullptr && socket->kind == rv::world::SocketKind::SideWall;
    });
    REQUIRE(standing != nullptr);
    const Fixture decoration = *standing;
    const Fixture display = *fixtureOf(plan, "room:snes/fx:dc01");

    SECTION("a decoration moved out of its socket and turned by 15 degrees") {
        const rv::world::TemplateSocket* socket = socketOf(decoration);
        const bool alongZ = socket->wall == rv::world::Wall::West || socket->wall == rv::world::Wall::East;
        std::optional<EditResult> accepted;
        std::string refusals;
        std::int64_t x = decoration.xMm;
        std::int64_t z = decoration.zMm;
        // Out of its socket into the room (a turn of 15 degrees against the wall would put a corner through it).
        const std::int64_t inward = socket->wall == rv::world::Wall::West ? 1 : -1;
        std::vector<std::pair<std::int64_t, std::int64_t>> candidates;
        for (const std::int64_t out : {600, 800, 1000, 1200}) {
            for (const std::int64_t step : {0, 100, -100, 500, -500}) {
                candidates.emplace_back(decoration.xMm + (alongZ ? inward * out : step),
                                        decoration.zMm + (alongZ ? step : -out));
            }
        }
        for (const auto& [cx, cz] : candidates) {
            x = cx;
            z = cz;
            EditedWorld trial = fresh.world;
            const EditResult result =
                edit(trial, EditOperation::moveFixture(decoration.id, x, z, decoration.yawCdeg + 1500), data);
            if (!result.refused) {
                accepted = result;
                fresh.world = trial;
                break;
            }
            refusals += result.refused->rule + " " + result.refused->subject + ": " + result.refused->message + "\n";
        }
        INFO(refusals);
        REQUIRE(accepted.has_value());
        CHECK(accepted->rebuild == rv::world::RebuildLevel::LocalReflow);
        CHECK(accepted->fixturesMoved == std::vector<FixtureId>{decoration.id});
        const Fixture* moved = fixtureOf(fresh.world.plan, decoration.id.toString());
        CHECK(moved->xMm == x);
        CHECK(moved->zMm == z);
        CHECK(moved->yawCdeg == decoration.yawCdeg + 1500);
        CHECK(moved->yMm == decoration.yMm);
        REQUIRE(fresh.world.overrides.fixturePins.size() == 1);
        const rv::world::FixturePin& pin = fresh.world.overrides.fixturePins.front();
        CHECK(pin.fixture == decoration.id);
        CHECK(pin.transform == rv::world::FixtureTransform{x, z, decoration.yawCdeg + 1500});
        CHECK(pin.provenance == kProvenance);
        requirePinsHonoured(fresh.world);
        // A lamp's light followed it (a turn of 15 degrees keeps its bulb's quarter turn).
        const FixtureId lightId = *FixtureId::make(decoration.id.room(), decoration.id.socket(), "light");
        for (const rv::world::PlanLight& before : fresh.generated.plan.lights) {
            if (before.id != lightId) {
                continue;
            }
            const auto after = std::find_if(fresh.world.plan.lights.begin(), fresh.world.plan.lights.end(),
                                            [&](const rv::world::PlanLight& light) { return light.id == lightId; });
            REQUIRE(after != fresh.world.plan.lights.end());
            CHECK(after->xMm - before.xMm == x - decoration.xMm);
            CHECK(after->zMm - before.zMm == z - decoration.zMm);
            CHECK(after->yMm == before.yMm);
        }
        // Put back home: its pin removed (it stands where the layout stands it), the plan as generated, its digest
        // included; nothing left to unpin.
        const EditResult back = edit(
            fresh.world, EditOperation::moveFixture(decoration.id, decoration.xMm, decoration.zMm, decoration.yawCdeg),
            data);
        REQUIRE_FALSE(back.refused);
        CHECK(back.changed);
        CHECK(fresh.world.overrides.empty());
        CHECK(fresh.world.plan == fresh.generated.plan);
        CHECK_FALSE(edit(fresh.world, EditOperation::unpinFixture(decoration.id), data).changed);
        // A move onto where it stands at home changes nothing, and a pin in its socket stays.
        CHECK_FALSE(edit(fresh.world, EditOperation::moveFixture(decoration.id, decoration.xMm, decoration.zMm), data)
                        .changed);
        REQUIRE(edit(fresh.world, EditOperation::pinFixture(decoration.id), data).changed);
        CHECK_FALSE(edit(fresh.world, EditOperation::moveFixture(decoration.id, decoration.xMm, decoration.zMm), data)
                        .changed);
        CHECK(fresh.world.overrides.fixturePins.size() == 1);
    }
    SECTION("pin and unpin a fixture where it stands") {
        CHECK(edit(fresh.world, EditOperation::pinFixture(display.id), data).changed);
        REQUIRE(fresh.world.overrides.fixturePins.size() == 1);
        CHECK_FALSE(fresh.world.overrides.fixturePins.front().transform.has_value());
        CHECK_FALSE(edit(fresh.world, EditOperation::pinFixture(display.id), data).changed);
        CHECK(updated(fresh.world, fresh.selection).plan == fresh.world.plan);
        CHECK(edit(fresh.world, EditOperation::unpinFixture(display.id), data).changed);
        CHECK_FALSE(edit(fresh.world, EditOperation::unpinFixture(display.id), data).changed);
        CHECK(fresh.world.plan == fresh.generated.plan);
    }
    SECTION("refusals by the edit's rules") {
        EditedWorld& world = fresh.world;
        requireRefused(world, EditOperation::moveFixture(*FixtureId::make(room.id, "nowhere"), 0, 0), data,
                       rv::world::edit_rule::kFixtureUnknown);
        const Fixture* hero = findFixture(plan, FixtureRole::HeroModule, [](const Fixture&) { return true; });
        requireRefused(world, EditOperation::moveFixture(hero->id, hero->xMm, hero->zMm + 100), data,
                       rv::world::edit_rule::kFixtureFixed);
        const Fixture crt = *fixtureOf(plan, "room:snes/fx:hero.crt");
        requireRefused(world, EditOperation::pinFixture(crt.id), data, rv::world::edit_rule::kFixtureFixed);
        requireRefused(world, EditOperation::moveFixture(display.id, display.xMm + 50, display.zMm), data,
                       rv::world::edit_rule::kOffGrid);
        requireRefused(world, EditOperation::moveFixture(display.id, display.xMm, display.zMm, 4500), data,
                       rv::world::edit_rule::kTurnStep);
        requireRefused(world, EditOperation::moveFixture(decoration.id, decoration.xMm, decoration.zMm, 1000), data,
                       rv::world::edit_rule::kTurnStep);
        // A room kept as an older template made it: its fixtures stay.
        EditedWorld kept = world;
        kept.plan.rooms.front().templateVersion = 1;
        requireRefused(kept, EditOperation::moveFixture(display.id, display.xMm + 100, display.zMm), data,
                       rv::world::edit_rule::kFixtureInKeptRoom);
    }
    SECTION("refusals by the hard rules") {
        EditedWorld& world = fresh.world;
        const rv::world::TemplateSocket* socket = socketOf(decoration);
        // Through its wall: outside the room.
        const std::int64_t outward = socket->wall == rv::world::Wall::West ? -1000 : 1000;
        requireRefused(world, EditOperation::moveFixture(decoration.id, decoration.xMm + outward, decoration.zMm),
                       data, rv::world::rule::kFixtureOutsideRoom);
        // Onto what stands in the next socket on its wall (the side-wall sockets stand 1 m apart), when that stands on
        // the floor too.
        const rv::world::RoomTemplate& roomTemplate = snesData().roomTemplate;
        for (const rv::world::TemplateSocket& other : roomTemplate.sockets) {
            if (other.wall != socket->wall || other.kind != rv::world::SocketKind::SideWall ||
                (other.zMm - socket->zMm != 1000 && other.zMm - socket->zMm != -1000) || other.growthStep > 0) {
                continue;
            }
            const Fixture* neighbour = fixtureOf(plan, "room:snes/fx:" + other.id);
            if (neighbour != nullptr && neighbour->yMm == 0) {
                requireRefused(world,
                               EditOperation::moveFixture(decoration.id, decoration.xMm,
                                                          decoration.zMm + other.zMm - socket->zMm),
                               data, rv::world::rule::kFootprintsOverlap);
                break;
            }
        }
        // A display case into the doorway's clear area, and into a door-reserved socket.
        const std::int64_t doorwayZ = room.maxZMm - 700;
        const std::int64_t toDoorway = ((doorwayZ - display.zMm) / 100) * 100;
        requireRefused(world, EditOperation::moveFixture(display.id, display.xMm + 1000, display.zMm + toDoorway, 0),
                       data, rv::world::rule::kDoorwayClearance);
        // Turned along the west wall in front of its door-reserved socket (x -4470 to -4070, z -400 to 1400).
        requireRefused(world, EditOperation::moveFixture(display.id, display.xMm - 3200, display.zMm - 500), data,
                       rv::world::rule::kDoorReserved);
    }
}

TEST_CASE("world/edit: an edit is refused only for a hard finding it causes", "[world][edit]") {
    Fresh fresh = freshRoom(80, 57);
    const EditData data = snesEditData();
    EditedWorld& world = fresh.world;
    // A plan written by hand with a hard finding: a decoration of the west wall pushed through it.
    const Fixture* standing = findFixture(world.plan, FixtureRole::Decoration, [](const Fixture& fixture) {
        const auto* socket = socketOf(fixture);
        return fixture.yMm == 0 && socket != nullptr && socket->wall == rv::world::Wall::West;
    });
    REQUIRE(standing != nullptr);
    const Fixture broken = *standing;
    for (Fixture& fixture : world.plan.fixtures) {
        if (fixture.id == broken.id) {
            fixture.xMm -= 1000;
        }
    }
    const std::vector<Placement> spine = placementsIn(world.plan, rv::world::Section::Spine);
    const std::vector<SlotId> free = freeSlots(world.plan, rv::world::Section::Spine, false);
    REQUIRE(spine.size() > 1);
    REQUIRE(free.size() > 1);

    // A game moved elsewhere on the floor is not refused for it; the validation still reports it.
    const EditResult moved = edit(world, EditOperation::move(spine[0].game, free[0]), data);
    INFO((moved.refused ? moved.refused->rule + " " + moved.refused->message : std::string("applied")));
    REQUIRE_FALSE(moved.refused);
    CHECK_FALSE(moved.validation.passed());
    // A session too (the floor's findings before it read once).
    const std::vector<EditOperation> session{EditOperation::move(spine[1].game, free[1]),
                                             EditOperation::pin(spine[0].game)};
    auto applied = rv::world::applyEdits(world, session, data, kProvenance);
    REQUIRE(applied.has_value());
    CHECK_FALSE(applied->refusedAt.has_value());
    // An edit that causes a hard finding of its own is still refused by it.
    const Fixture display = *fixtureOf(world.plan, "room:snes/fx:dc01");
    const std::int64_t toDoorway = ((world.plan.rooms.front().maxZMm - 700 - display.zMm) / 100) * 100;
    requireRefused(world, EditOperation::moveFixture(display.id, display.xMm + 1000, display.zMm + toDoorway, 0),
                   data, rv::world::rule::kDoorwayClearance);
    // The fixture at fault put back home: the floor passes again, its pin removed.
    const EditResult repaired = edit(world, EditOperation::moveFixture(broken.id, broken.xMm, broken.zMm), data);
    REQUIRE_FALSE(repaired.refused);
    CHECK(repaired.validation.passed());
}

TEST_CASE("world/edit: a hung decoration slides along the walls only", "[world][edit]") {
    // A room whose decoration includes a poster (the decoration is drawn from each socket's seed).
    std::optional<Fresh> found;
    Fixture poster;
    for (std::uint64_t seed = 1; seed < 40 && !found; ++seed) {
        Fresh fresh = freshRoom(40, seed);
        const Fixture* hung = findFixture(fresh.world.plan, FixtureRole::Decoration, [](const Fixture& fixture) {
            const auto* socket = socketOf(fixture);
            return fixture.yMm > 0 && socket != nullptr && socket->kind == rv::world::SocketKind::SideWall;
        });
        if (hung != nullptr) {
            poster = *hung;
            found = std::move(fresh);
        }
    }
    REQUIRE(found.has_value());
    const EditData data = snesEditData();
    EditedWorld& world = found->world;
    const bool west = socketOf(poster)->wall == rv::world::Wall::West;
    // Off its wall into the room: refused, a turn that does not face into the room too.
    requireRefused(world, EditOperation::moveFixture(poster.id, poster.xMm + (west ? 500 : -500), poster.zMm), data,
                   rv::world::edit_rule::kWallMount);
    requireRefused(world, EditOperation::moveFixture(poster.id, poster.xMm, poster.zMm, poster.yawCdeg + 1500), data,
                   rv::world::edit_rule::kWallMount);
    requireRefused(world, EditOperation::moveFixture(poster.id, poster.xMm, poster.zMm + 50), data,
                   rv::world::edit_rule::kOffGrid);
    // Along its wall, the coordinate across it exempt from the grid but on the wall: a poster hangs over nothing that
    // stands below it, so a few steps either way find a place.
    bool slid = false;
    for (const std::int64_t step : {100, -100, 200, -200, 300, -300}) {
        EditedWorld trial = world;
        const EditResult result = edit(trial, EditOperation::moveFixture(poster.id, poster.xMm, poster.zMm + step), data);
        if (!result.refused) {
            slid = true;
            requirePinsHonoured(trial);
            break;
        }
        CHECK(result.refused->rule != rv::world::edit_rule::kWallMount);
    }
    CHECK(slid);
}

TEST_CASE("world/edit: an update with additions keeps every pin and moved fixture; a socket a moved fixture "
          "overlaps is skipped",
          "[world][edit]") {
    Fresh fresh = freshRoom(120, 41);
    const EditData data = snesEditData();
    EditedWorld& world = fresh.world;
    const rv::world::RoomTemplate& roomTemplate = snesData().roomTemplate;
    // The side-wall sockets in the collection's order that hold decoration: the next modules' sockets.
    std::vector<std::string> next;
    for (const std::string& socket : roomTemplate.collectionOrder) {
        const Fixture* fixture = fixtureOf(world.plan, "room:snes/fx:" + socket);
        if (fixture != nullptr && fixture->role == FixtureRole::Decoration && fixture->yMm == 0) {
            next.push_back(socket);
        }
    }
    REQUIRE(next.size() >= 3);
    // The decoration of the socket after the next one, w, slid into the next socket's place (its own decoration taken
    // out of the plan first, as if none fitted there), pinned there.
    const std::string skipped = next.front();
    const Fixture* source = nullptr;
    for (const std::string& socket : next) {
        const Fixture* candidate = fixtureOf(world.plan, "room:snes/fx:" + socket);
        if (socket != skipped && roomTemplate.socket(socket)->wall == roomTemplate.socket(skipped)->wall) {
            source = source == nullptr ? candidate : source;
        }
    }
    REQUIRE(source != nullptr);
    const Fixture moving = *source;
    const std::int64_t shift = roomTemplate.socket(skipped)->zMm - roomTemplate.socket(moving.id.socket())->zMm;
    std::erase_if(world.plan.fixtures, [&](const Fixture& fixture) { return fixture.id.socket() == skipped; });
    std::erase_if(world.plan.lights, [&](const rv::world::PlanLight& light) { return light.id.socket() == skipped; });
    const EditResult slid = edit(world, EditOperation::moveFixture(moving.id, moving.xMm, moving.zMm + shift), data);
    REQUIRE_FALSE(slid.refused);
    // A game moved into another free slot and another pinned in place.
    const std::vector<Placement> spines = placementsIn(world.plan, rv::world::Section::Spine);
    const std::vector<Placement> bands = placementsIn(world.plan, rv::world::Section::Band);
    REQUIRE_FALSE(spines.empty());
    REQUIRE_FALSE(bands.empty());
    const Placement spine = spines.front();
    const std::vector<SlotId> reserve = freeSlots(world.plan, rv::world::Section::Spine, false);
    REQUIRE_FALSE(reserve.empty());
    const SlotId display = reserve.back();
    REQUIRE_FALSE(edit(world, EditOperation::move(spine.game, display), data).refused);
    const Placement band = bands.front();
    REQUIRE(edit(world, EditOperation::pin(band.game), data).changed);
    REQUIRE(world.overrides.gamePins.size() == 2);
    REQUIRE(world.overrides.fixturePins.size() == 1);

    // Many titles added: modules in the sockets the moved decoration does not stand over, the room grown.
    RoomSelection more = fresh.selection;
    rv::core::Random random(rv::core::deriveSeed("edit-additions", 41));
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(more);
    for (int i = 0; i < 2500; ++i) {
        rv::test::world::addTitle(more, random, taken);
    }
    const GenerationResult update = updated(world, more);
    INFO((update.validation.findings.empty() ? std::string()
                                             : update.validation.findings.front().rule + " " +
                                                   update.validation.findings.front().subject + ": " +
                                                   update.validation.findings.front().message));
    CHECK(update.validation.passed());
    CHECK_FALSE(update.changes.added.empty());
    // Every pin stands (§21.3): the games in their slots, the moved decoration where it was put.
    EditedWorld after{update.plan, world.overrides};
    requirePinsHonoured(after);
    const Fixture* stays = fixtureOf(update.plan, moving.id.toString());
    REQUIRE(stays != nullptr);
    CHECK(stays->xMm == moving.xMm);
    CHECK(stays->zMm == moving.zMm + shift);
    // The socket it stands over took no module, and the report says so.
    const Fixture* overSkipped = fixtureOf(update.plan, "room:snes/fx:" + skipped);
    CHECK((overSkipped == nullptr || overSkipped->role != FixtureRole::SideModule));
    for (const std::string& line : update.changes.needsConfirmation) {
        UNSCOPED_INFO("needs confirmation: " << line);
    }
    UNSCOPED_INFO("added " << update.changes.added.size() << ", fixtures added " << update.changes.fixturesAdded.size());
    const std::string note = "socket " + skipped + ": a side-wall module would overlap " + moving.id.toString() +
                             ", moved there; it went to the next socket";
    CHECK(std::find(update.changes.needsConfirmation.begin(), update.changes.needsConfirmation.end(), note) !=
          update.changes.needsConfirmation.end());
    // The next update of the same selection writes nothing.
    const GenerationResult again = updateRoom(update.plan, more, kWorldSeed, world.overrides);
    CHECK(again.plan == update.plan);
}

TEST_CASE("world/edit: a fixture moved by the user is never moved by the room's growth", "[world][edit]") {
    // A room with decoration on the south wall (a decoration socket the density chose).
    std::optional<Fresh> found;
    Fixture south;
    for (std::uint64_t seed = 1; seed < 40 && !found; ++seed) {
        Fresh fresh = freshRoom(60, seed);
        const Fixture* onSouth = findFixture(fresh.world.plan, FixtureRole::Decoration, [](const Fixture& fixture) {
            const auto* socket = socketOf(fixture);
            return socket != nullptr && socket->wall == rv::world::Wall::South && fixture.yMm == 0;
        });
        if (onSouth != nullptr) {
            south = *onSouth;
            found = std::move(fresh);
        }
    }
    REQUIRE(found.has_value());
    const EditData data = snesEditData();
    EditedWorld& world = found->world;
    bool moved = false;
    for (const std::int64_t step : {100, -100, 200, -200}) {
        EditedWorld trial = world;
        if (!edit(trial, EditOperation::moveFixture(south.id, south.xMm + step, south.zMm), data).refused) {
            world = trial;
            moved = true;
            break;
        }
    }
    REQUIRE(moved);
    const rv::world::Fixture placed = *fixtureOf(world.plan, south.id.toString());
    RoomSelection more = found->selection;
    rv::core::Random random(rv::core::deriveSeed("edit-growth", 1));
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(more);
    for (int i = 0; i < 6000; ++i) {
        rv::test::world::addTitle(more, random, taken);
    }
    const GenerationResult update = updated(world, more);
    INFO((update.changes.needsConfirmation.empty() ? std::string("") : update.changes.needsConfirmation.front()));
    REQUIRE(update.plan.rooms.front().growthSteps > world.plan.rooms.front().growthSteps);
    CHECK(update.changes.rebuild == rv::world::RebuildLevel::RoomExpansion);
    const Fixture* stays = fixtureOf(update.plan, south.id.toString());
    REQUIRE(stays != nullptr);
    CHECK(stays->xMm == placed.xMm);
    CHECK(stays->zMm == placed.zMm);
    // The south wall's other fixtures moved with it.
    for (const Fixture& fixture : world.plan.fixtures) {
        const auto* socket = socketOf(fixture);
        if (fixture.id != south.id && socket != nullptr && socket->wall == rv::world::Wall::South) {
            const Fixture* after = fixtureOf(update.plan, fixture.id.toString());
            REQUIRE(after != nullptr);
            CHECK(after->zMm > fixture.zMm);
        }
    }
    EditedWorld after{update.plan, world.overrides};
    requirePinsHonoured(after);
    INFO((update.validation.findings.empty() ? std::string()
                                             : update.validation.findings.front().rule + " " +
                                                   update.validation.findings.front().message));
    CHECK(update.validation.passed());
    // A fixture pinned in its socket of the south wall still refuses the growth (as before I4b).
    EditedWorld socketPinned{found->generated.plan, {}};
    REQUIRE(edit(socketPinned, EditOperation::pinFixture(south.id), data).changed);
    const GenerationResult refused = updated(socketPinned, more);
    CHECK(refused.plan.rooms.front().growthSteps == socketPinned.plan.rooms.front().growthSteps);
    CHECK_FALSE(refused.changes.needsConfirmation.empty());
}

TEST_CASE("world/edit: a south-wall fixture moved back home is unpinned and moves with its wall again",
          "[world][edit]") {
    std::optional<Fresh> found;
    Fixture south;
    for (std::uint64_t seed = 1; seed < 40 && !found; ++seed) {
        Fresh fresh = freshRoom(60, seed);
        const Fixture* onSouth = findFixture(fresh.world.plan, FixtureRole::Decoration, [](const Fixture& fixture) {
            const auto* socket = socketOf(fixture);
            return socket != nullptr && socket->wall == rv::world::Wall::South && fixture.yMm == 0;
        });
        if (onSouth != nullptr) {
            south = *onSouth;
            found = std::move(fresh);
        }
    }
    REQUIRE(found.has_value());
    const EditData data = snesEditData();
    EditedWorld& world = found->world;
    bool moved = false;
    for (const std::int64_t step : {100, -100, 200, -200}) {
        EditedWorld trial = world;
        if (!edit(trial, EditOperation::moveFixture(south.id, south.xMm + step, south.zMm), data).refused) {
            world = trial;
            moved = true;
            break;
        }
    }
    REQUIRE(moved);
    REQUIRE(world.overrides.fixturePins.size() == 1);
    const EditResult back = edit(world, EditOperation::moveFixture(south.id, south.xMm, south.zMm), data);
    REQUIRE_FALSE(back.refused);
    CHECK(world.overrides.empty());
    CHECK(world.plan == found->generated.plan);

    // The growth is taken (a pin in its socket would refuse it) and the fixture moves with the south wall.
    RoomSelection more = found->selection;
    rv::core::Random random(rv::core::deriveSeed("edit-growth", 1));
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(more);
    for (int i = 0; i < 6000; ++i) {
        rv::test::world::addTitle(more, random, taken);
    }
    const GenerationResult update = updated(world, more);
    const std::uint32_t steps = update.plan.rooms.front().growthSteps - world.plan.rooms.front().growthSteps;
    REQUIRE(steps > 0);
    const Fixture* after = fixtureOf(update.plan, south.id.toString());
    REQUIRE(after != nullptr);
    CHECK(after->xMm == south.xMm);
    CHECK(after->zMm == south.zMm + static_cast<std::int64_t>(steps) * snesData().roomTemplate.growthStepMm);
    CHECK(update.validation.passed());
}

namespace {

/// A fresh room of `titles` titles whose decoration includes a poster that `accept` takes, the world's seed varied
/// (each socket's decoration is drawn from the room's seed), with that poster and that seed.
struct PosterRoom {
    Fresh fresh;
    Fixture poster;
    std::uint64_t worldSeed = 0;
};
template <class Accept>
std::optional<PosterRoom> roomWithPoster(std::size_t titles, Accept&& accept) {
    for (std::uint64_t worldSeed = kWorldSeed; worldSeed < kWorldSeed + 200; ++worldSeed) {
        PosterRoom room;
        room.worldSeed = worldSeed;
        room.fresh.selection = makeSelection(titles, 1);
        room.fresh.generated = generateSelection(room.fresh.selection, worldSeed);
        REQUIRE(room.fresh.generated.validation.passed());
        room.fresh.world = EditedWorld{room.fresh.generated.plan, {}};
        const Fixture* poster =
            findFixture(room.fresh.world.plan, FixtureRole::Decoration, [&](const Fixture& fixture) {
                return fixture.yMm > 0 && socketOf(fixture) != nullptr && accept(*socketOf(fixture));
            });
        if (poster != nullptr) {
            room.poster = *poster;
            return room;
        }
    }
    return std::nullopt;
}

/// `selection` with `count` titles more (seeded by `name`).
RoomSelection withMore(RoomSelection selection, std::string_view name, int count) {
    rv::core::Random random(rv::core::deriveSeed(name, 1));
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(selection);
    for (int i = 0; i < count; ++i) {
        rv::test::world::addTitle(selection, random, taken);
    }
    return selection;
}

/// The poster moved by `update` follows the south wall: grown `steps` times, it stands `steps` growth steps south of
/// `placed`, its pin rewritten in the update's overrides to where it stands, every pin honoured, the plan valid.
void requireFollowedWall(const EditedWorld& world, const GenerationResult& update, const Fixture& placed) {
    const std::uint32_t steps = update.plan.rooms.front().growthSteps - world.plan.rooms.front().growthSteps;
    REQUIRE(steps > 0);
    const std::int64_t shift = static_cast<std::int64_t>(steps) * snesData().roomTemplate.growthStepMm;
    const Fixture* after = fixtureOf(update.plan, placed.id.toString());
    REQUIRE(after != nullptr);
    CHECK(after->xMm == placed.xMm);
    CHECK(after->zMm == placed.zMm + shift);
    CHECK(after->yawCdeg == placed.yawCdeg);
    REQUIRE(update.overrides.has_value());
    REQUIRE(update.overrides->fixturePins.size() == 1);
    const rv::world::FixturePin& pin = update.overrides->fixturePins.front();
    CHECK(pin.transform == rv::world::FixtureTransform{placed.xMm, placed.zMm + shift, placed.yawCdeg});
    CHECK(pin.provenance == kProvenance);
    requirePinsHonoured(EditedWorld{update.plan, *update.overrides});
    INFO((update.validation.findings.empty() ? std::string()
                                             : update.validation.findings.front().rule + " " +
                                                   update.validation.findings.front().message));
    CHECK(update.validation.passed());
    // No growth refused (the socket a poster left takes no module: that is reported).
    for (const std::string& line : update.changes.needsConfirmation) {
        INFO(line);
        CHECK(line.rfind("growth:", 0) != 0);
    }
}

} // namespace

TEST_CASE("world/edit: a fixture's home and the place snapped to its grid", "[world][edit]") {
    auto found = roomWithPoster(40, [](const rv::world::TemplateSocket& socket) {
        return socket.kind == rv::world::SocketKind::SideWall && socket.wall == rv::world::Wall::West;
    });
    REQUIRE(found.has_value());
    EditedWorld& world = found->fresh.world;
    const Fixture poster = found->poster;
    const EditData data = snesEditData(found->worldSeed);
    const rv::world::Room room = world.plan.rooms.front();
    const Fixture display = *fixtureOf(world.plan, "room:snes/fx:dc01");
    using rv::world::FixtureTransform;

    // At home a fixture stands where the layout stood it; fixed or unknown fixtures have none.
    CHECK(rv::world::fixtureHome(world, display.id, data) ==
          FixtureTransform{display.xMm, display.zMm, display.yawCdeg});
    const Fixture* hero = findFixture(world.plan, FixtureRole::HeroModule, [](const Fixture&) { return true; });
    CHECK_FALSE(rv::world::fixtureHome(world, hero->id, data).has_value());
    CHECK_FALSE(rv::world::fixtureHome(world, *FixtureId::make(room.id, "nowhere"), data).has_value());
    CHECK_FALSE(rv::world::snapFixture(world, hero->id, {0, 0, 0}, data).has_value());

    // A display case: the yaw to a quarter turn, x and z to the grid through its home.
    const auto snapped = rv::world::snapFixture(world, display.id, {display.xMm + 149, display.zMm - 51, 8000}, data);
    CHECK(snapped == FixtureTransform{display.xMm + 100, display.zMm - 100, 9000});
    CHECK(rv::world::snapFixture(world, display.id, {display.xMm - 250, display.zMm + 250, 35000}, data) ==
          FixtureTransform{display.xMm - 300, display.zMm + 300, 0});
    // Moved, its home stays.
    REQUIRE_FALSE(
        edit(world, EditOperation::moveFixture(display.id, snapped->xMm, snapped->zMm, snapped->yawCdeg), data).refused);
    CHECK(rv::world::fixtureHome(world, display.id, data) ==
          FixtureTransform{display.xMm, display.zMm, display.yawCdeg});

    // A poster along its own wall: on the grid through its home, its back where it was.
    CHECK(rv::world::snapFixture(world, poster.id, {poster.xMm + 700, poster.zMm + 130, 4500}, data) ==
          FixtureTransform{poster.xMm, poster.zMm + 100, poster.yawCdeg});
    // Near the south wall: onto it, facing north, its back as far off it as at home (30 mm), its x on the grid; the
    // edit's own rules accept the place.
    const auto onSouth = rv::world::snapFixture(world, poster.id, {poster.xMm + 1240, room.maxZMm - 300, 0}, data);
    REQUIRE(onSouth.has_value());
    CHECK(*onSouth == FixtureTransform{poster.xMm + 1200, room.maxZMm - 30, 18000});
    for (const auto& wanted : {FixtureTransform{room.maxXMm - 10, 0, 0}, FixtureTransform{0, room.minZMm + 5, 0},
                               FixtureTransform{-20000, -20000, 0}}) {
        const auto place = rv::world::snapFixture(world, poster.id, wanted, data);
        REQUIRE(place.has_value());
        EditedWorld trial = world;
        const EditResult result =
            edit(trial, EditOperation::moveFixture(poster.id, place->xMm, place->zMm, place->yawCdeg), data);
        INFO(place->xMm << " " << place->zMm << " " << place->yawCdeg << ": "
                        << (result.refused ? result.refused->rule + " " + result.refused->message : "accepted"));
        if (result.refused) {
            for (const rv::world::ValidationRule& own : rv::world::editRules()) {
                CHECK(result.refused->rule != own.name);
            }
            CHECK(result.refused->rule != rv::world::rule::kFixtureOutsideRoom);
        }
    }
}

TEST_CASE("world/edit: a poster moved along or onto the south wall follows it when the room grows, its pin too",
          "[world][edit]") {
    SECTION("slid along the south wall from dec-sw or dec-se") {
        auto found = roomWithPoster(60, [](const rv::world::TemplateSocket& socket) {
            return socket.id == "dec-sw" || socket.id == "dec-se";
        });
        REQUIRE(found.has_value());
        Fresh& fresh = found->fresh;
        const Fixture poster = found->poster;
        const std::uint64_t seed = found->worldSeed;
        const EditData data = snesEditData(seed);
        bool slid = false;
        for (const std::int64_t step : {100, -100, 200, -200, 300, -300}) {
            EditedWorld trial = fresh.world;
            if (!edit(trial, EditOperation::moveFixture(poster.id, poster.xMm + step, poster.zMm), data).refused) {
                fresh.world = trial;
                slid = true;
                break;
            }
        }
        REQUIRE(slid);
        const Fixture placed = *fixtureOf(fresh.world.plan, poster.id.toString());
        const GenerationResult update =
            updateRoom(fresh.world.plan, withMore(fresh.selection, "edit-poster", 6000), seed, fresh.world.overrides);
        requireFollowedWall(fresh.world, update, placed);
        // With its pin rewritten, the next update of the same selection writes nothing.
        const GenerationResult again =
            updateRoom(update.plan, withMore(fresh.selection, "edit-poster", 6000), seed, *update.overrides);
        CHECK(again.plan == update.plan);
        CHECK_FALSE(again.overrides.has_value());
    }
    SECTION("moved onto the south wall from a side wall") {
        auto found = roomWithPoster(60, [](const rv::world::TemplateSocket& socket) {
            return socket.kind == rv::world::SocketKind::SideWall && socket.wall == rv::world::Wall::West;
        });
        REQUIRE(found.has_value());
        Fresh& fresh = found->fresh;
        const Fixture poster = found->poster;
        const std::uint64_t seed = found->worldSeed;
        const EditData data = snesEditData(seed);
        const rv::world::Room& room = fresh.world.plan.rooms.front();
        // Its back 30 mm off the south wall (as on its socket), along the wall on the grid through its home's x.
        bool moved = false;
        std::string refusals;
        for (const std::int64_t x : {600, 700, 800, 500, 900}) {
            EditedWorld trial = fresh.world;
            const EditResult result = edit(
                trial, EditOperation::moveFixture(poster.id, poster.xMm + x, room.maxZMm - 30, 18000), data);
            if (!result.refused) {
                fresh.world = trial;
                moved = true;
                break;
            }
            refusals += result.refused->rule + ": " + result.refused->message + "\n";
        }
        INFO(refusals);
        REQUIRE(moved);
        const Fixture placed = *fixtureOf(fresh.world.plan, poster.id.toString());
        const GenerationResult update =
            updateRoom(fresh.world.plan, withMore(fresh.selection, "edit-poster", 6000), seed, fresh.world.overrides);
        requireFollowedWall(fresh.world, update, placed);
    }
    SECTION("a poster left in mid-air is a hard finding") {
        auto found = roomWithPoster(60, [](const rv::world::TemplateSocket& socket) {
            return socket.id == "dec-sw" || socket.id == "dec-se";
        });
        REQUIRE(found.has_value());
        Fresh& fresh = found->fresh;
        const Fixture poster = found->poster;
        const std::uint64_t seed = found->worldSeed;
        WorldPlan plan = fresh.world.plan;
        for (Fixture& fixture : plan.fixtures) {
            if (fixture.id == poster.id) {
                fixture.zMm -= 2000;
            }
        }
        // The update judges the plan it keeps (a plan written by hand): the poster held where it stands, refused.
        const rv::world::ValidationReport report = updateRoom(plan, fresh.selection, seed, {}).validation;
        CHECK(std::any_of(report.findings.begin(), report.findings.end(), [&](const auto& finding) {
            return finding.rule == rv::world::rule::kDecorationOffWall && finding.subject == poster.id.toString();
        }));
    }
}

TEST_CASE("world/edit: a fixture moved then unpinned is still held where it stands by growth", "[world][edit]") {
    // A room with decoration on the south wall (a decoration socket the density chose).
    std::optional<Fresh> found;
    Fixture south;
    for (std::uint64_t seed = 1; seed < 40 && !found; ++seed) {
        Fresh fresh = freshRoom(60, seed);
        const Fixture* onSouth = findFixture(fresh.world.plan, FixtureRole::Decoration, [](const Fixture& fixture) {
            const auto* socket = socketOf(fixture);
            return socket != nullptr && socket->wall == rv::world::Wall::South && fixture.yMm == 0;
        });
        if (onSouth != nullptr) {
            south = *onSouth;
            found = std::move(fresh);
        }
    }
    REQUIRE(found.has_value());
    const EditData data = snesEditData();
    EditedWorld& world = found->world;
    bool moved = false;
    for (const std::int64_t step : {100, -100, 200, -200}) {
        EditedWorld trial = world;
        if (!edit(trial, EditOperation::moveFixture(south.id, south.xMm + step, south.zMm), data).refused) {
            world = trial;
            moved = true;
            break;
        }
    }
    REQUIRE(moved);
    REQUIRE(edit(world, EditOperation::unpinFixture(south.id), data).changed);
    REQUIRE(world.overrides.empty());
    const Fixture placed = *fixtureOf(world.plan, south.id.toString());
    CHECK(placed.xMm != south.xMm);

    RoomSelection more = found->selection;
    rv::core::Random random(rv::core::deriveSeed("edit-growth", 1));
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(more);
    for (int i = 0; i < 6000; ++i) {
        rv::test::world::addTitle(more, random, taken);
    }
    const GenerationResult update = updated(world, more);
    REQUIRE(update.plan.rooms.front().growthSteps > world.plan.rooms.front().growthSteps);
    // No pin, yet it stands where the user left it: "moved" follows where a fixture stands, not its pin.
    const Fixture* stays = fixtureOf(update.plan, south.id.toString());
    REQUIRE(stays != nullptr);
    CHECK(stays->xMm == placed.xMm);
    CHECK(stays->zMm == placed.zMm);
    INFO((update.validation.findings.empty() ? std::string()
                                             : update.validation.findings.front().rule + " " +
                                                   update.validation.findings.front().message));
    CHECK(update.validation.passed());
    CHECK(updateRoom(update.plan, more, kWorldSeed, world.overrides).plan == update.plan);
}

TEST_CASE("world/edit: the wall-washers follow where modules stand, pinned or not", "[world][edit]") {
    // A room whose west wall holds modules in w01 and w03 (1,200 titles fill the side-wall sockets past w03).
    const Fresh found = freshRoom(1200, 17);
    for (const std::string_view socket : {"room:snes/fx:w01", "room:snes/fx:w03"}) {
        const Fixture* module = fixtureOf(found.world.plan, socket);
        REQUIRE(module != nullptr);
        REQUIRE(module->role == FixtureRole::SideModule);
    }
    const EditData data = snesEditData();
    // Each module stood out into the room, facing it as before (the first place the rules accept).
    const auto standOut = [&](EditedWorld& world, std::string_view id) {
        const Fixture module = *fixtureOf(world.plan, id);
        for (const std::int64_t out : {1000, 1200, 800, 1400}) {
            for (const std::int64_t along : {0, 100, -100}) {
                EditedWorld trial = world;
                if (!edit(trial, EditOperation::moveFixture(module.id, module.xMm + out, module.zMm + along), data)
                         .refused) {
                    world = trial;
                    return true;
                }
            }
        }
        return false;
    };
    EditedWorld pinned = found.world;
    REQUIRE(standOut(pinned, "room:snes/fx:w03"));
    EditedWorld unpinned = pinned;
    REQUIRE(edit(unpinned, EditOperation::unpinFixture(*FixtureId::parse("room:snes/fx:w03")), data).changed);
    CHECK(unpinned.plan.lights == pinned.plan.lights);
    REQUIRE(standOut(pinned, "room:snes/fx:w01"));
    REQUIRE(standOut(unpinned, "room:snes/fx:w01"));
    CHECK(fixtureOf(unpinned.plan, "room:snes/fx:w01")->xMm == fixtureOf(pinned.plan, "room:snes/fx:w01")->xMm);
    // The washers placed again for w01 see w03 where it stands, with its pin or without: none aims at its socket.
    CHECK(unpinned.plan.lights == pinned.plan.lights);
    const rv::world::TemplateSocket& w03 = *snesData().roomTemplate.socket("w03");
    for (const rv::world::PlanLight& light : unpinned.plan.lights) {
        if (light.role != rv::world::LightRole::WallWasher || light.targetXMm > w03.xMm + 500) {
            continue;
        }
        INFO(light.id.toString() << " aims at z " << light.targetZMm << ", span " << light.spanMm);
        const std::int64_t from = light.targetZMm - light.spanMm / 2;
        const std::int64_t to = light.targetZMm + light.spanMm / 2;
        CHECK((to <= w03.zMm - 450 || from >= w03.zMm + 450));
    }
}

TEST_CASE("world/edit: random edit sessions keep the plan valid, every manual change pinned, the update writing "
          "nothing",
          "[world][edit][property]") {
    const EditData data = snesEditData();
    for (const std::uint64_t seed : {7ull, 8ull, 9ull}) {
        Fresh fresh = freshRoom(70, seed);
        EditedWorld& world = fresh.world;
        rv::core::Random random(rv::core::deriveSeed("edit-property", seed));
        std::vector<Fixture> movable;
        for (const Fixture& fixture : world.plan.fixtures) {
            if (fixture.role == FixtureRole::SideModule || fixture.role == FixtureRole::Display ||
                fixture.role == FixtureRole::Decoration) {
                movable.push_back(fixture);
            }
        }
        std::vector<SlotId> slots;
        for (const Fixture& fixture : world.plan.fixtures) {
            for (const rv::world::FixtureLevel& level : fixture.levels) {
                for (std::uint32_t index = 0; index < level.slots; ++index) {
                    slots.push_back(*SlotId::make(fixture.id, level.level, index));
                }
            }
        }
        std::size_t applied = 0;
        for (int step = 0; step < 40; ++step) {
            const std::vector<Placement>& placements = world.plan.placements;
            const Placement game = placements[random.below(placements.size())];
            const Placement other = placements[random.below(placements.size())];
            EditOperation operation;
            switch (random.below(6)) {
            case 0:
            case 1:
                operation = EditOperation::move(game.game, slots[random.below(slots.size())]);
                break;
            case 2:
                operation = EditOperation::swap(game.game, other.game);
                break;
            case 3:
                operation = random.below(2) == 0 ? EditOperation::pin(game.game) : EditOperation::unpin(game.game);
                break;
            default: {
                const Fixture& base = movable[random.below(movable.size())];
                const Fixture* now = fixtureOf(world.plan, base.id.toString());
                const auto offset = [&] { return (static_cast<std::int64_t>(random.below(9)) - 4) * 100; };
                const std::int32_t turn = base.role == FixtureRole::Decoration ? 1500 : 9000;
                const auto yaw = (now->yawCdeg + turn * static_cast<std::int32_t>(random.below(4))) % 36000;
                operation = EditOperation::moveFixture(base.id, now->xMm + offset(), now->zMm + offset(), yaw);
                break;
            }
            }
            const EditedWorld before = world;
            const EditResult result = edit(world, operation, data);
            if (result.refused) {
                CHECK(world == before);
                continue;
            }
            ++applied;
            if (!result.rooms.empty()) {
                CHECK(result.validation.passed());
            }
            // Every game moved and fixture moved is pinned where it stands.
            if (operation.kind == EditKind::Move || operation.kind == EditKind::Swap) {
                for (const GameId& moved : {operation.game, operation.with}) {
                    if (!moved.isValid()) {
                        continue;
                    }
                    const auto pin = std::find_if(world.overrides.gamePins.begin(), world.overrides.gamePins.end(),
                                                  [&](const auto& candidate) { return candidate.game == moved; });
                    REQUIRE(pin != world.overrides.gamePins.end());
                    CHECK(placementOf(world.plan, moved)->slot == pin->slot);
                }
            }
            if (operation.kind == EditKind::MoveFixture) {
                // Pinned where it stands, unless back home (where the fresh layout stood it), where it stands unpinned.
                const auto pin =
                    std::find_if(world.overrides.fixturePins.begin(), world.overrides.fixturePins.end(),
                                 [&](const auto& candidate) { return candidate.fixture == operation.fixture; });
                const Fixture* now = fixtureOf(world.plan, operation.fixture.toString());
                const Fixture* home = fixtureOf(fresh.generated.plan, operation.fixture.toString());
                const bool atHome =
                    now->xMm == home->xMm && now->zMm == home->zMm && now->yawCdeg == home->yawCdeg;
                CHECK((pin != world.overrides.fixturePins.end()) != atHome);
            }
            requirePinsHonoured(world);
        }
        CHECK(applied > 5);
        // The whole plan validates with its pins, and the update of the same selection writes nothing.
        const GenerationResult update = updated(world, fresh.selection);
        CHECK(update.validation.passed());
        CHECK(update.plan == world.plan);
        CHECK(update.changes.digestAfter == update.changes.digestBefore);
    }
}

TEST_CASE("world/edit: the edit script's format", "[world][edit]") {
    const GameId game = *GameId::fromValue(0x0123456789abcdefull);
    const GameId with = *GameId::fromValue(0x42);
    const SlotId slot = *SlotId::parse("room:snes/fx:dc01/s:0.1");
    const FixtureId fixture = *FixtureId::parse("room:snes/fx:w03");
    const std::vector<EditOperation> operations{
        EditOperation::move(game, slot),
        EditOperation::swap(game, with),
        EditOperation::pin(game),
        EditOperation::unpin(with),
        EditOperation::moveFixture(fixture, -1200, 300, 9000),
        EditOperation::moveFixture(fixture, 0, -100),
        EditOperation::pinFixture(fixture),
        EditOperation::unpinFixture(fixture),
    };
    const std::string text = rv::world::writeEditScript(operations);
    auto read = rv::world::parseEditScript(text, "script.json");
    INFO((read ? std::string() : read.error().toString()));
    REQUIRE(read.has_value());
    CHECK(*read == operations);
    CHECK(rv::world::writeEditScript(*read) == text);
    CHECK(rv::world::editText(operations[4]) == "move-fixture room:snes/fx:w03 to x -1200 z 300 mm, yaw 9000 cdeg");

    const auto refused = [](std::string_view script) {
        auto parsed = rv::world::parseEditScript(script, "script.json");
        return parsed ? std::optional<rv::Error>() : std::optional(parsed.error());
    };
    // Comments are allowed, as in every hand-written file.
    CHECK_FALSE(refused("// a session\n{\"schemaVersion\": 1, \"operations\": []}"));
    CHECK(refused("{\"schemaVersion\": 2, \"operations\": []}")->code == rv::ErrorCode::Unsupported);
    CHECK(refused("{\"schemaVersion\": 1, \"operations\": [{\"op\": \"hide\", \"game\": \"g0000000000000042\"}]}")
              ->code == rv::ErrorCode::ValidationFailed);
    CHECK(refused("{\"schemaVersion\": 1, \"operations\": [{\"op\": \"move\", \"game\": \"g0000000000000042\"}]}"));
    CHECK(refused("{\"schemaVersion\": 1, \"operations\": [{\"op\": \"pin\", \"game\": \"g0000000000000042\", "
                  "\"slot\": \"room:snes/fx:dc01/s:0.1\"}]}"));
    CHECK(refused("{\"schemaVersion\": 1, \"operations\": [{\"op\": \"pin\", \"game\": \"G42\"}]}"));
    CHECK(refused("{\"schemaVersion\": 1, \"operations\": [{\"op\": \"move-fixture\", \"fixture\": "
                  "\"room:snes/fx:w03\", \"xMm\": 0, \"zMm\": 0, \"yawCdeg\": 36000}]}"));
    CHECK(refused("{\"schemaVersion\": 1, \"operations\": [{\"op\": \"move-fixture\", \"fixture\": "
                  "\"room:snes/fx:w03\", \"xMm\": 0.5, \"zMm\": 0}]}"));
    CHECK(refused("{\"schemaVersion\": 1}"));
}
