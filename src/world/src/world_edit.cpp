#include <rv/world/world_edit.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/world/incremental.hpp>
#include <rv/world/plan_json.hpp>

#include "allocation.hpp"
#include "data_reader.hpp"
#include "fixture_layout.hpp"
#include "generation_parts.hpp"

namespace rv::world {
namespace {

using catalog::GameId;
using Json = nlohmann::json;

constexpr std::array<ValidationRule, 11> kEditRules{{
    {edit_rule::kGameUnknown, Severity::Hard,
     "A game moved, swapped or pinned stands in a slot of the plan (an edit places no unplaced game)."},
    {edit_rule::kSlotTaken, Severity::Hard, "A game moves into an empty slot (onto another game, it swaps with it)."},
    {edit_rule::kSlotPinned, Severity::Hard,
     "A slot pinned to another game takes no other game and no other pin until that pin is removed."},
    {edit_rule::kOtherRoom, Severity::Hard,
     "A game moves only within its room chain: its system's room and that room's expansions."},
    {edit_rule::kSameGame, Severity::Hard, "A game swaps with another game."},
    {edit_rule::kFixtureUnknown, Severity::Hard,
     "A fixture moved or pinned is one of the plan's, in a socket of its room's template, its prefab measured."},
    {edit_rule::kFixtureFixed, Severity::Hard,
     "Only side-wall modules, display fixtures and decoration move or are pinned; the hero corner and the hero-wall "
     "modules are fixed."},
    {edit_rule::kRoomKept, Severity::Hard, "No fixture moves in a room kept as an older template made it."},
    {edit_rule::kOffGrid, Severity::Hard,
     "A fixture's pivot stays on the room's 100 mm grid through its place in its socket (a hung decoration's along "
     "its wall)."},
    {edit_rule::kTurnStep, Severity::Hard,
     "A fixture turns by quarter turns, decoration by 15 degrees, its yaw from 0 to 35999 cdeg."},
    {edit_rule::kWallMount, Severity::Hard,
     "A decoration hung on a wall stays on a wall: its back on a wall's face (within 100 mm), facing into the room."},
}};

/// A decoration hung on a wall (a poster): it stands above the floor.
bool isHung(const Fixture& fixture) noexcept {
    return fixture.role == FixtureRole::Decoration && fixture.yMm > 0;
}

std::string transformText(std::int64_t xMm, std::int64_t zMm, std::int32_t yawCdeg) {
    return fmt::format("x {} z {} mm, yaw {} cdeg", xMm, zMm, yawCdeg);
}

/// The pins the plan honours.
WorldOverrides honouredPins(const WorldPlan& plan, const WorldOverrides& overrides) {
    return checkPins(plan, {}, overrides).honoured;
}

/// One edit applied to a world (a copy: applyEdit() keeps it only when the edit passes).
class Editor {
public:
    Editor(EditedWorld& world, const EditData& data, const PinProvenance& provenance)
        : m_world(world), m_plan(world.plan), m_data(data), m_provenance(provenance) {}

    Expected<EditResult> run(const EditOperation& operation) {
        switch (operation.kind) {
        case EditKind::Move:
            return moveGame(operation.game, operation.slot);
        case EditKind::Swap:
            return swapGames(operation.game, operation.with);
        case EditKind::Pin:
            return pinGame(operation.game);
        case EditKind::Unpin:
            return unpinGame(operation.game);
        case EditKind::MoveFixture:
            return moveFixture(operation);
        case EditKind::PinFixture:
            return pinFixture(operation.fixture);
        case EditKind::UnpinFixture:
            return unpinFixture(operation.fixture);
        }
        return makeError(ErrorCode::InvalidArgument, "an unknown edit", "applyEdit");
    }

private:
    static EditResult refusal(std::string_view rule, std::string subject, std::string message) {
        EditResult result;
        result.refused = EditRefusal{std::string(rule), std::move(subject), std::move(message)};
        return result;
    }

    static EditResult unchanged(std::string summary) {
        EditResult result;
        result.summary = std::move(summary);
        return result;
    }

    // --- Lookups --------------------------------------------------------------------------------------------------

    Placement* placementOf(const GameId& game) {
        const auto found = std::find_if(m_plan.placements.begin(), m_plan.placements.end(),
                                        [&](const Placement& placement) { return placement.game == game; });
        return found != m_plan.placements.end() ? &*found : nullptr;
    }
    Placement* placementIn(const SlotId& slot) {
        const auto found = std::find_if(m_plan.placements.begin(), m_plan.placements.end(),
                                        [&](const Placement& placement) { return placement.slot == slot; });
        return found != m_plan.placements.end() ? &*found : nullptr;
    }
    Fixture* fixtureOf(const FixtureId& id) {
        const auto found = std::find_if(m_plan.fixtures.begin(), m_plan.fixtures.end(),
                                        [&](const Fixture& fixture) { return fixture.id == id; });
        return found != m_plan.fixtures.end() ? &*found : nullptr;
    }
    const Room* roomOf(const RoomId& id) const {
        const auto found = std::find_if(m_plan.rooms.begin(), m_plan.rooms.end(),
                                        [&](const Room& room) { return room.id == id; });
        return found != m_plan.rooms.end() ? &*found : nullptr;
    }
    static const FixtureLevel* levelOf(const Fixture& fixture, std::uint32_t level) {
        const auto found = std::find_if(fixture.levels.begin(), fixture.levels.end(),
                                        [&](const FixtureLevel& candidate) { return candidate.level == level; });
        return found != fixture.levels.end() ? &*found : nullptr;
    }
    const FixtureLevel* slotLevel(const SlotId& slot) {
        const Fixture* fixture = fixtureOf(slot.fixture());
        if (fixture == nullptr) {
            return nullptr;
        }
        const FixtureLevel* level = levelOf(*fixture, slot.level());
        return level != nullptr && slot.index() < level->slots ? level : nullptr;
    }
    Expected<const WorldData*> dataOf(const RoomId& room) const {
        const auto found = m_data.rooms.find(room);
        if (found == m_data.rooms.end() || found->second == nullptr) {
            return makeError(ErrorCode::InvalidArgument, "the edit's data lacks the room " + room.toString(),
                             "applyEdit");
        }
        return found->second.get();
    }

    GamePin* gamePinOf(const GameId& game) {
        const auto found = std::find_if(m_world.overrides.gamePins.begin(), m_world.overrides.gamePins.end(),
                                        [&](const GamePin& pin) { return pin.game == game; });
        return found != m_world.overrides.gamePins.end() ? &*found : nullptr;
    }
    const GamePin* slotPinOf(const SlotId& slot) const {
        const auto found = std::find_if(m_world.overrides.gamePins.begin(), m_world.overrides.gamePins.end(),
                                        [&](const GamePin& pin) { return pin.slot == slot; });
        return found != m_world.overrides.gamePins.end() ? &*found : nullptr;
    }
    FixturePin* fixturePinOf(const FixtureId& fixture) {
        const auto found = std::find_if(m_world.overrides.fixturePins.begin(), m_world.overrides.fixturePins.end(),
                                        [&](const FixturePin& pin) { return pin.fixture == fixture; });
        return found != m_world.overrides.fixturePins.end() ? &*found : nullptr;
    }

    void setGamePin(const GameId& game, const SlotId& slot) {
        std::erase_if(m_world.overrides.gamePins, [&](const GamePin& pin) { return pin.game == game; });
        m_world.overrides.gamePins.push_back({game, slot, m_provenance});
        std::sort(m_world.overrides.gamePins.begin(), m_world.overrides.gamePins.end(),
                  [](const GamePin& a, const GamePin& b) { return a.game < b.game; });
    }

    void setFixturePin(const FixtureId& fixture, std::optional<FixtureTransform> transform) {
        std::optional<PrefabRef> prefab;
        if (const FixturePin* existing = fixturePinOf(fixture)) {
            prefab = existing->prefab; // a pinned prefab stays pinned
        }
        std::erase_if(m_world.overrides.fixturePins, [&](const FixturePin& pin) { return pin.fixture == fixture; });
        m_world.overrides.fixturePins.push_back({fixture, std::move(prefab), transform, m_provenance});
        std::sort(m_world.overrides.fixturePins.begin(), m_world.overrides.fixturePins.end(),
                  [](const FixturePin& a, const FixturePin& b) { return a.fixture < b.fixture; });
    }

    /// The pins of `game`'s other slot or another game's pin of `slot`: a slot holds one pin (overrides.hpp).
    std::optional<EditResult> slotPinnedByOther(const SlotId& slot, const GameId& game, const GameId& other = {}) {
        const GamePin* pin = slotPinOf(slot);
        if (pin != nullptr && pin->game != game && pin->game != other) {
            return refusal(edit_rule::kSlotPinned, slot.toString(),
                           fmt::format("the slot is pinned to {}; unpin it first", pin->game.toString()));
        }
        return std::nullopt;
    }

    // --- Games ------------------------------------------------------------------------------------------------------

    /// A placement's offset as a fresh plan draws it in its slot (allocation.hpp): spine-out pushed in, face-out turned.
    void drawOffset(Placement& placement) {
        const Room* room = roomOf(placement.slot.fixture().room());
        const FixtureLevel* level = slotLevel(placement.slot);
        if (room != nullptr && level != nullptr) {
            detail::setPlacementOffset(placement, room->seed, room->mediaFamily, isFaceOut(level->section));
        }
    }

    /// The dressing on `slot` gives way: each span covering it split around it. True when one did.
    bool giveWay(const SlotId& slot) {
        const LevelId level = slot.levelId();
        std::vector<DressedLevel> kept;
        bool changed = false;
        for (DressedLevel& dressed : m_plan.dressing) {
            if (dressed.level != level || slot.index() < dressed.firstSlot ||
                slot.index() >= dressed.firstSlot + dressed.count) {
                kept.push_back(std::move(dressed));
                continue;
            }
            changed = true;
            if (slot.index() > dressed.firstSlot) {
                kept.push_back({dressed.level, dressed.firstSlot, slot.index() - dressed.firstSlot, dressed.prop});
            }
            const std::uint32_t end = dressed.firstSlot + dressed.count;
            if (slot.index() + 1 < end) {
                kept.push_back({dressed.level, slot.index() + 1, end - slot.index() - 1, dressed.prop});
            }
        }
        m_plan.dressing = std::move(kept);
        return changed;
    }

    /// A face-out slot a game left is dressed with one slot of its level's prop (as an update frees one: the face-out
    /// set shows no gap); a spine-out one stays free (the reserve). True when dressed.
    Expected<bool> dressFreed(const SlotId& slot) {
        const FixtureLevel* level = slotLevel(slot);
        const Room* room = roomOf(slot.fixture().room());
        if (level == nullptr || room == nullptr || !isFaceOut(level->section)) {
            return false;
        }
        auto data = dataOf(room->id);
        if (!data) {
            return propagate(data.error());
        }
        const std::vector<PrefabRef>& props = (*data)->roomTemplate.shelfDressing;
        if (props.empty()) {
            return false;
        }
        m_plan.dressing.push_back(
            {slot.levelId(), slot.index(), 1, detail::dressingProp(room->seed, slot.levelId(), props)});
        return true;
    }

    Expected<EditResult> moveGame(const GameId& game, const SlotId& slot) {
        Placement* from = placementOf(game);
        if (from == nullptr) {
            return refusal(edit_rule::kGameUnknown, game.toString(),
                           "the game stands in no slot of the plan: an edit moves only placed games");
        }
        if (slotLevel(slot) == nullptr) {
            return refusal(rule::kSlotUnknown, slot.toString(), "the plan has no such slot");
        }
        if (from->slot == slot) {
            return pinGame(game);
        }
        const RoomId chain = from->slot.fixture().room().head();
        if (slot.fixture().room().head() != chain) {
            return refusal(edit_rule::kOtherRoom, slot.toString(),
                           fmt::format("{} stands in {}: it moves only within the rooms of {}", game.toString(),
                                       from->slot.fixture().room().toString(), chain.toString()));
        }
        if (const Placement* there = placementIn(slot)) {
            return refusal(edit_rule::kSlotTaken, slot.toString(),
                           fmt::format("the slot holds {}: swap them instead", there->game.toString()));
        }
        if (auto pinned = slotPinnedByOther(slot, game)) {
            return std::move(*pinned);
        }
        const SlotId left = from->slot;
        bool dressing = giveWay(slot);
        from = placementOf(game);
        from->slot = slot;
        drawOffset(*from);
        auto dressed = dressFreed(left);
        if (!dressed) {
            return propagate(dressed.error());
        }
        dressing = dressing || *dressed;
        setGamePin(game, slot);
        EditResult result;
        result.changed = true;
        result.rebuild = dressing ? RebuildLevel::LocalReflow : RebuildLevel::NoRebuild;
        result.rooms = {left.fixture().room(), slot.fixture().room()};
        result.summary =
            fmt::format("moved {} from {} to {}, pinned there", game.toString(), left.toString(), slot.toString());
        return result;
    }

    Expected<EditResult> swapGames(const GameId& game, const GameId& with) {
        if (game == with) {
            return refusal(edit_rule::kSameGame, game.toString(), "a game swaps with another game");
        }
        Placement* first = placementOf(game);
        Placement* second = placementOf(with);
        if (first == nullptr || second == nullptr) {
            const GameId& missing = first == nullptr ? game : with;
            return refusal(edit_rule::kGameUnknown, missing.toString(),
                           "the game stands in no slot of the plan: an edit moves only placed games");
        }
        if (first->slot.fixture().room().head() != second->slot.fixture().room().head()) {
            return refusal(edit_rule::kOtherRoom, with.toString(),
                           fmt::format("{} stands in {} and {} in {}: games swap only within one room chain",
                                       game.toString(), first->slot.fixture().room().toString(), with.toString(),
                                       second->slot.fixture().room().toString()));
        }
        if (auto pinned = slotPinnedByOther(second->slot, game, with)) {
            return std::move(*pinned);
        }
        if (auto pinned = slotPinnedByOther(first->slot, game, with)) {
            return std::move(*pinned);
        }
        std::swap(first->slot, second->slot);
        drawOffset(*first);
        drawOffset(*second);
        const SlotId firstSlot = first->slot;
        const SlotId secondSlot = second->slot;
        setGamePin(game, firstSlot);
        setGamePin(with, secondSlot);
        EditResult result;
        result.changed = true;
        result.rooms = {firstSlot.fixture().room(), secondSlot.fixture().room()};
        result.summary = fmt::format("swapped {} (now in {}) and {} (now in {}), both pinned", game.toString(),
                                     firstSlot.toString(), with.toString(), secondSlot.toString());
        return result;
    }

    Expected<EditResult> pinGame(const GameId& game) {
        const Placement* placement = placementOf(game);
        if (placement == nullptr) {
            return refusal(edit_rule::kGameUnknown, game.toString(),
                           "the game stands in no slot of the plan: only a placed game is pinned in place");
        }
        if (auto pinned = slotPinnedByOther(placement->slot, game)) {
            return std::move(*pinned);
        }
        const GamePin* existing = gamePinOf(game);
        if (existing != nullptr && existing->slot == placement->slot) {
            return unchanged(fmt::format("{} is pinned in {} already", game.toString(), placement->slot.toString()));
        }
        const SlotId slot = placement->slot;
        setGamePin(game, slot);
        EditResult result;
        result.changed = true;
        result.summary = fmt::format("pinned {} in {}", game.toString(), slot.toString());
        return result;
    }

    Expected<EditResult> unpinGame(const GameId& game) {
        const GamePin* existing = gamePinOf(game);
        if (existing == nullptr) {
            return unchanged(fmt::format("{} has no pin", game.toString()));
        }
        const SlotId slot = existing->slot;
        std::erase_if(m_world.overrides.gamePins, [&](const GamePin& pin) { return pin.game == game; });
        EditResult result;
        result.changed = true;
        result.summary = fmt::format("unpinned {} (pinned to {})", game.toString(), slot.toString());
        return result;
    }

    // --- Fixtures ---------------------------------------------------------------------------------------------------

    /// What a movable fixture's edit reads: its room, its data, its socket, its prefab, its place in its socket.
    struct FixtureContext {
        Fixture* fixture = nullptr;
        const Room* room = nullptr;
        const WorldData* data = nullptr;
        const TemplateSocket* socket = nullptr;
        const PrefabInfo* info = nullptr;
        detail::SocketPose home;
    };

    /// The fixture's context, or why it cannot be edited (`refused`), or an error.
    Expected<std::optional<EditResult>> fixtureContext(const FixtureId& id, FixtureContext& context) {
        context.fixture = fixtureOf(id);
        if (context.fixture == nullptr) {
            return std::optional(
                refusal(edit_rule::kFixtureUnknown, id.toString(), "the plan holds no such fixture"));
        }
        if (!detail::isMovableRole(context.fixture->role)) {
            return std::optional(refusal(
                edit_rule::kFixtureFixed, id.toString(),
                fmt::format("a {} fixture is fixed: only side-wall modules, display fixtures and decoration move",
                            toString(context.fixture->role))));
        }
        context.room = roomOf(id.room());
        if (context.room == nullptr) {
            return std::optional(
                refusal(edit_rule::kFixtureUnknown, id.toString(), "the plan does not hold the fixture's room"));
        }
        auto data = dataOf(context.room->id);
        if (!data) {
            return propagate(data.error());
        }
        context.data = *data;
        if (std::string reason = roomKeepReason(m_plan, *context.room, *context.data); !reason.empty()) {
            return std::optional(refusal(rule::kRoomKept, context.room->id.toString(),
                                         "the room is kept as an older template made it until a regeneration: " +
                                             reason));
        }
        context.socket = context.data->roomTemplate.socket(id.socket());
        context.info = context.data->prefabs.find(context.fixture->prefab);
        if (context.socket == nullptr || context.info == nullptr) {
            return std::optional(refusal(edit_rule::kFixtureUnknown, id.toString(),
                                         context.socket == nullptr
                                             ? "the room's template has no socket " + id.socket()
                                             : "the fixture's prefab " + context.fixture->prefab.manifest +
                                                   " is not among the room's measured prefabs"));
        }
        context.home = detail::poseInSocket(layoutInput(context), *context.socket, *context.info);
        return std::optional<EditResult>();
    }

    detail::LayoutInput layoutInput(const FixtureContext& context) const {
        detail::LayoutInput input{context.data->roomTemplate,
                                  context.data->platform,
                                  context.data->mediaFamily,
                                  context.data->prefabs,
                                  context.room->id,
                                  context.room->seed,
                                  {},
                                  m_data.config.decorationDensityPermille};
        input.counts.growthSteps = context.room->growthSteps;
        return input;
    }

    /// The transform a fixture standing at `pose` is pinned with: none at its home (pinned in its socket).
    static std::optional<FixtureTransform> pinnedTransform(const FixtureContext& context,
                                                           const detail::SocketPose& pose) {
        if (pose == context.home) {
            return std::nullopt;
        }
        return FixtureTransform{pose.xMm, pose.zMm, pose.yawCdeg};
    }

    /// The fixture's place checked against the edit's rules (grid, turn, wall); a refusal or nothing.
    static std::optional<EditResult> checkPlace(const FixtureContext& context, const detail::SocketPose& pose) {
        const Fixture& fixture = *context.fixture;
        const std::string subject = fixture.id.toString();
        if (pose.yawCdeg < 0 || pose.yawCdeg >= 36000) {
            return refusal(edit_rule::kTurnStep, subject,
                           fmt::format("the yaw {} cdeg is not from 0 to 35999", pose.yawCdeg));
        }
        const std::int32_t step = fixture.role == FixtureRole::Decoration ? kDecorationTurnCdeg : kFixtureTurnCdeg;
        if (pose.yawCdeg % step != 0) {
            return refusal(edit_rule::kTurnStep, subject,
                           fmt::format("the yaw {} cdeg is not a multiple of {} cdeg: a {} turns by {} degrees",
                                       pose.yawCdeg, step, toString(fixture.role), step / 100));
        }
        if (std::abs(pose.xMm) > kMaxFixtureOffsetMm || std::abs(pose.zMm) > kMaxFixtureOffsetMm) {
            return refusal(rule::kFixtureOutsideRoom, subject,
                           fmt::format("x {} z {} mm lies outside the room", pose.xMm, pose.zMm));
        }
        const auto offGrid = [&](std::int64_t value, std::int64_t home) { return (value - home) % kEditGridMm != 0; };
        if (!isHung(fixture)) {
            if (offGrid(pose.xMm, context.home.xMm) || offGrid(pose.zMm, context.home.zMm)) {
                return refusal(edit_rule::kOffGrid, subject,
                               fmt::format("x {} z {} mm is off the room's {} mm grid through the fixture's place in "
                                           "its socket (x {} z {} mm)",
                                           pose.xMm, pose.zMm, kEditGridMm, context.home.xMm, context.home.zMm));
            }
            return std::nullopt;
        }
        // A hung decoration: its back on the wall it faces into the room from, along that wall on the grid.
        if (pose.yawCdeg % kFixtureTurnCdeg != 0) {
            return refusal(edit_rule::kWallMount, subject,
                           fmt::format("a decoration hung on a wall faces into the room from it: yaw 0, 9000, 18000 or "
                                       "27000 cdeg, not {}",
                                       pose.yawCdeg));
        }
        const Room& room = *context.room;
        const std::int64_t back = context.info->minZMm; // the back's offset along the front from the pivot
        std::int64_t gap = 0;
        bool alongX = true;
        std::string_view wall;
        switch (pose.yawCdeg) {
        case 0:
            gap = pose.zMm + back - room.minZMm;
            wall = "north";
            break;
        case 9000:
            gap = pose.xMm + back - room.minXMm;
            alongX = false;
            wall = "west";
            break;
        case 18000:
            gap = room.maxZMm - (pose.zMm - back);
            wall = "south";
            break;
        default:
            gap = room.maxXMm - (pose.xMm - back);
            alongX = false;
            wall = "east";
            break;
        }
        if (gap < 0 || gap > kMaxWallGapMm) {
            return refusal(edit_rule::kWallMount, subject,
                           fmt::format("its back stands {} mm from the {} wall it faces away from; a hung decoration's "
                                       "back stands on a wall, 0 to {} mm off it",
                                       gap, wall, kMaxWallGapMm));
        }
        const bool off = alongX ? offGrid(pose.xMm, context.home.xMm) : offGrid(pose.zMm, context.home.zMm);
        if (off) {
            return refusal(edit_rule::kOffGrid, subject,
                           fmt::format("{} {} mm along the {} wall is off the room's {} mm grid through the fixture's "
                                       "place in its socket ({} {} mm)",
                                       alongX ? "x" : "z", alongX ? pose.xMm : pose.zMm, wall, kEditGridMm,
                                       alongX ? "x" : "z", alongX ? context.home.xMm : context.home.zMm));
        }
        return std::nullopt;
    }

    /// The modules of `room` in the collection's order (the update's modulesInOrder()).
    std::vector<const Fixture*> modulesOf(const Room& room, const RoomTemplate& roomTemplate) const {
        std::vector<const Fixture*> modules;
        for (const Fixture& fixture : m_plan.fixtures) {
            if (fixture.id.room() == room.id &&
                (fixture.role == FixtureRole::HeroModule || fixture.role == FixtureRole::SideModule)) {
                modules.push_back(&fixture);
            }
        }
        const auto rank = [&](const Fixture* fixture) {
            const auto& order = roomTemplate.collectionOrder;
            return static_cast<std::size_t>(std::find(order.begin(), order.end(), fixture->id.socket()) -
                                            order.begin());
        };
        std::stable_sort(modules.begin(), modules.end(), [&](const Fixture* a, const Fixture* b) {
            return std::pair(rank(a), a->id) < std::pair(rank(b), b->id);
        });
        return modules;
    }

    /// The room's wall-washers placed again over its modules where they stand (the update's rule, incremental.cpp).
    void placeWallWashersAgain(const FixtureContext& context) {
        const Room& room = *context.room;
        std::erase_if(m_plan.lights, [&](const PlanLight& light) {
            return light.id.room() == room.id && light.role == LightRole::WallWasher;
        });
        const detail::LayoutInput input = layoutInput(context);
        const std::vector<const Fixture*> modules = modulesOf(room, context.data->roomTemplate);
        // Every module is judged where it stands (moved: elsewhere than its home, pinned or not).
        std::set<FixtureId> moved;
        for (const Fixture* module : modules) {
            moved.insert(module->id);
        }
        const detail::ModulesOnWalls onWalls = detail::modulesOnWalls(input, room, modules, moved);
        const std::size_t limit = std::min(context.data->roomTemplate.lightLimit, kMaxSceneLights);
        const auto others = static_cast<std::size_t>(
            std::count_if(m_plan.lights.begin(), m_plan.lights.end(),
                          [&](const PlanLight& light) { return light.id.room() == room.id; }));
        const std::size_t budget = limit - std::min(limit, others);
        for (const PlanLight& light : detail::wallWasherLights(input, onWalls.modules, budget)) {
            m_plan.lights.push_back(light);
        }
    }

    Expected<EditResult> moveFixture(const EditOperation& operation) {
        FixtureContext context;
        auto refused = fixtureContext(operation.fixture, context);
        if (!refused) {
            return propagate(refused.error());
        }
        if (*refused) {
            return std::move(**refused);
        }
        Fixture& fixture = *context.fixture;
        const detail::SocketPose pose{operation.xMm, operation.zMm, operation.yawCdeg.value_or(fixture.yawCdeg)};
        if (auto broken = checkPlace(context, pose)) {
            return std::move(*broken);
        }
        const std::optional<FixtureTransform> transform = pinnedTransform(context, pose);
        const detail::SocketPose before{fixture.xMm, fixture.zMm, fixture.yawCdeg};
        if (pose == before) {
            return pinFixture(operation.fixture);
        }
        fixture.xMm = pose.xMm;
        fixture.zMm = pose.zMm;
        fixture.yawCdeg = pose.yawCdeg;
        // A lamp's light follows it.
        if (context.info->lightSource) {
            const FixtureId lightId = *FixtureId::make(fixture.id.room(), fixture.id.socket(), "light");
            for (PlanLight& light : m_plan.lights) {
                if (light.id == lightId) {
                    light = detail::lampLightOf(fixture, *context.info, lightId);
                }
            }
        }
        setFixturePin(fixture.id, transform);
        if (fixture.role == FixtureRole::SideModule) {
            placeWallWashersAgain(context);
        }
        EditResult result;
        result.changed = true;
        result.rebuild = RebuildLevel::LocalReflow;
        result.rooms = {fixture.id.room()};
        result.fixturesMoved = {fixture.id};
        result.summary =
            transform ? fmt::format("moved {} to {}, pinned there", fixture.id.toString(),
                                    transformText(pose.xMm, pose.zMm, pose.yawCdeg))
                      : fmt::format("moved {} back into its socket, pinned in it", fixture.id.toString());
        return result;
    }

    Expected<EditResult> pinFixture(const FixtureId& id) {
        FixtureContext context;
        auto refused = fixtureContext(id, context);
        if (!refused) {
            return propagate(refused.error());
        }
        if (*refused) {
            return std::move(**refused);
        }
        const Fixture& fixture = *context.fixture;
        const std::optional<FixtureTransform> transform =
            pinnedTransform(context, {fixture.xMm, fixture.zMm, fixture.yawCdeg});
        if (const FixturePin* existing = fixturePinOf(id); existing != nullptr && existing->transform == transform) {
            return unchanged(fmt::format("{} is pinned already", id.toString()));
        }
        setFixturePin(id, transform);
        EditResult result;
        result.changed = true;
        result.summary = transform ? fmt::format("pinned {} at {}", id.toString(),
                                                 transformText(transform->xMm, transform->zMm, transform->yawCdeg))
                                   : fmt::format("pinned {} in its socket", id.toString());
        return result;
    }

    Expected<EditResult> unpinFixture(const FixtureId& id) {
        if (fixturePinOf(id) == nullptr) {
            return unchanged(fmt::format("{} has no pin", id.toString()));
        }
        std::erase_if(m_world.overrides.fixturePins, [&](const FixturePin& pin) { return pin.fixture == id; });
        EditResult result;
        result.changed = true;
        result.summary = fmt::format("unpinned {} (it stays where it stands)", id.toString());
        return result;
    }

    EditedWorld& m_world;
    WorldPlan& m_plan;
    const EditData& m_data;
    const PinProvenance& m_provenance;
};

/// `plan` with only the rooms of `floor` and their elements (every floor kept: the ground floor's rule).
WorldPlan floorPart(const WorldPlan& plan, const std::set<RoomId>& rooms) {
    WorldPlan part = plan;
    std::erase_if(part.rooms, [&](const Room& room) { return !rooms.contains(room.id); });
    std::erase_if(part.fixtures, [&](const Fixture& fixture) { return !rooms.contains(fixture.id.room()); });
    std::erase_if(part.placements,
                  [&](const Placement& placement) { return !rooms.contains(placement.slot.fixture().room()); });
    std::erase_if(part.dressing,
                  [&](const DressedLevel& dressed) { return !rooms.contains(dressed.level.fixture().room()); });
    std::erase_if(part.lights, [&](const PlanLight& light) { return !rooms.contains(light.id.room()); });
    std::erase_if(part.unplaced, [&](const UnplacedGame& game) { return !rooms.contains(game.room); });
    return part;
}

/// The validation of `floor` of the edited world: its rooms with their templates, kept rooms judged as kept, their
/// lights counted, the pins of its rooms that the plan honours or that are orphaned (an update's), the building's
/// halls. Errors: InvalidArgument for a room without data.
Expected<ValidationReport> validateFloor(const EditedWorld& world, const std::string& floor, const EditData& data) {
    std::set<RoomId> rooms;
    for (const Room& room : world.plan.rooms) {
        if (room.floor == floor) {
            rooms.insert(room.id);
        }
    }
    const WorldPlan part = floorPart(world.plan, rooms);
    std::vector<const WorldData*> datas;
    std::vector<RoomTemplate> templates;
    std::vector<KeptRoom> kept;
    for (const Room& room : part.rooms) {
        const auto found = data.rooms.find(room.id);
        if (found == data.rooms.end() || found->second == nullptr) {
            return makeError(ErrorCode::InvalidArgument, "the edit's data lacks the room " + room.id.toString(),
                             "applyEdit");
        }
        datas.push_back(found->second.get());
        templates.push_back(found->second->roomTemplate);
        if (std::string reason = roomKeepReason(world.plan, room, *found->second); !reason.empty()) {
            kept.push_back({room.id, std::move(reason)});
        }
    }
    WorldOverrides pins;
    for (const GamePin& pin : world.overrides.gamePins) {
        if (rooms.contains(pin.slot.fixture().room())) {
            pins.gamePins.push_back(pin);
        }
    }
    for (const FixturePin& pin : world.overrides.fixturePins) {
        if (rooms.contains(pin.fixture.room())) {
            pins.fixturePins.push_back(pin);
        }
    }
    const PinCheck check = checkPins(part, templates, pins);
    return validatePlan(part, detail::buildingValidationContext(part, data.config, {}, datas, data.building,
                                                                check.honouredOrOrphaned, std::move(kept)));
}

/// The floor of `room` in `plan`, or nothing.
std::optional<std::string> floorOf(const WorldPlan& plan, const RoomId& room) {
    for (const Room& candidate : plan.rooms) {
        if (candidate.id == room) {
            return candidate.floor;
        }
    }
    return std::nullopt;
}

// --- The script ------------------------------------------------------------------------------------------------------

Json operationJson(const EditOperation& operation) {
    Json json{{"op", std::string(toString(operation.kind))}};
    switch (operation.kind) {
    case EditKind::Move:
        json["game"] = operation.game.toString();
        json["slot"] = operation.slot.toString();
        break;
    case EditKind::Swap:
        json["game"] = operation.game.toString();
        json["with"] = operation.with.toString();
        break;
    case EditKind::Pin:
    case EditKind::Unpin:
        json["game"] = operation.game.toString();
        break;
    case EditKind::MoveFixture:
        json["fixture"] = operation.fixture.toString();
        json["xMm"] = operation.xMm;
        json["zMm"] = operation.zMm;
        if (operation.yawCdeg) {
            json["yawCdeg"] = *operation.yawCdeg;
        }
        break;
    case EditKind::PinFixture:
    case EditKind::UnpinFixture:
        json["fixture"] = operation.fixture.toString();
        break;
    }
    return json;
}

} // namespace

EditOperation EditOperation::move(const GameId& game, const SlotId& slot) {
    EditOperation operation;
    operation.kind = EditKind::Move;
    operation.game = game;
    operation.slot = slot;
    return operation;
}

EditOperation EditOperation::swap(const GameId& game, const GameId& with) {
    EditOperation operation;
    operation.kind = EditKind::Swap;
    operation.game = game;
    operation.with = with;
    return operation;
}

EditOperation EditOperation::pin(const GameId& game) {
    EditOperation operation;
    operation.kind = EditKind::Pin;
    operation.game = game;
    return operation;
}

EditOperation EditOperation::unpin(const GameId& game) {
    EditOperation operation;
    operation.kind = EditKind::Unpin;
    operation.game = game;
    return operation;
}

EditOperation EditOperation::moveFixture(const FixtureId& fixture, std::int64_t xMm, std::int64_t zMm,
                                         std::optional<std::int32_t> yawCdeg) {
    EditOperation operation;
    operation.kind = EditKind::MoveFixture;
    operation.fixture = fixture;
    operation.xMm = xMm;
    operation.zMm = zMm;
    operation.yawCdeg = yawCdeg;
    return operation;
}

EditOperation EditOperation::pinFixture(const FixtureId& fixture) {
    EditOperation operation;
    operation.kind = EditKind::PinFixture;
    operation.fixture = fixture;
    return operation;
}

EditOperation EditOperation::unpinFixture(const FixtureId& fixture) {
    EditOperation operation;
    operation.kind = EditKind::UnpinFixture;
    operation.fixture = fixture;
    return operation;
}

std::string editText(const EditOperation& operation) {
    switch (operation.kind) {
    case EditKind::Move:
        return fmt::format("move {} to {}", operation.game.toString(), operation.slot.toString());
    case EditKind::Swap:
        return fmt::format("swap {} with {}", operation.game.toString(), operation.with.toString());
    case EditKind::Pin:
    case EditKind::Unpin:
        return fmt::format("{} {}", toString(operation.kind), operation.game.toString());
    case EditKind::MoveFixture:
        return fmt::format("move-fixture {} to x {} z {} mm{}", operation.fixture.toString(), operation.xMm,
                           operation.zMm,
                           operation.yawCdeg ? fmt::format(", yaw {} cdeg", *operation.yawCdeg) : std::string());
    case EditKind::PinFixture:
    case EditKind::UnpinFixture:
        return fmt::format("{} {}", toString(operation.kind), operation.fixture.toString());
    }
    return "unknown";
}

std::span<const ValidationRule> editRules() noexcept {
    return kEditRules;
}

Expected<EditResult> applyEdit(EditedWorld& world, const EditOperation& operation, const EditData& data,
                               const PinProvenance& provenance) {
    EditedWorld edited = world;
    auto applied = Editor(edited, data, provenance).run(operation);
    if (!applied) {
        return propagate(applied.error());
    }
    EditResult result = std::move(*applied);
    if (result.refused || !result.changed) {
        return result;
    }
    sortPlan(edited.plan);
    // The plan's elements changed: the floor of the rooms changed is judged by the hard rules (game moves stay within
    // a chain, whose rooms share a floor).
    if (!result.rooms.empty()) {
        std::sort(result.rooms.begin(), result.rooms.end());
        result.rooms.erase(std::unique(result.rooms.begin(), result.rooms.end()), result.rooms.end());
        const std::optional<std::string> floor = floorOf(edited.plan, result.rooms.front());
        if (floor) {
            auto validation = validateFloor(edited, *floor, data);
            if (!validation) {
                return propagate(validation.error());
            }
            // The refusal names the first hard finding about what the edit moved (a fixture moved into a doorway
            // also leaves the floor's hall unreachable, a rule that runs earlier), else the first hard finding.
            std::set<std::string> subjects;
            for (const FixtureId& fixture : result.fixturesMoved) {
                subjects.insert(fixture.toString());
            }
            const ValidationFinding* first = nullptr;
            for (const ValidationFinding& finding : validation->findings) {
                if (finding.severity != Severity::Hard) {
                    continue;
                }
                if (first == nullptr || (subjects.contains(finding.subject) && !subjects.contains(first->subject))) {
                    first = &finding;
                }
            }
            if (first != nullptr) {
                EditResult refused;
                refused.refused = EditRefusal{first->rule, first->subject, first->message};
                refused.floor = *floor;
                refused.validation = std::move(*validation);
                return refused;
            }
            result.floor = *floor;
            result.validation = std::move(*validation);
        }
    }
    // The honoured pins' term of the inputDigest replaced (ADR-0015 §2).
    auto before = detail::overridesDigestTerm(honouredPins(world.plan, world.overrides));
    auto after = detail::overridesDigestTerm(honouredPins(edited.plan, edited.overrides));
    if (!before || !after) {
        return propagate((!before ? before : after).error());
    }
    auto digest = detail::joinDigestTerm(edited.plan.inputDigest, *before);
    if (digest) {
        digest = detail::joinDigestTerm(*digest, *after);
    }
    if (!digest) {
        return propagate(digest.error());
    }
    edited.plan.inputDigest = std::move(*digest);
    world = std::move(edited);
    return result;
}

Expected<EditSession> applyEdits(EditedWorld& world, std::span<const EditOperation> operations, const EditData& data,
                                 const PinProvenance& provenance) {
    EditedWorld edited = world;
    EditSession session;
    for (std::size_t i = 0; i < operations.size(); ++i) {
        auto result = applyEdit(edited, operations[i], data, provenance);
        if (!result) {
            return propagate(result.error());
        }
        const bool refused = result->refused.has_value();
        session.results.push_back(std::move(*result));
        if (refused) {
            session.refusedAt = i;
            return session;
        }
    }
    world = std::move(edited);
    return session;
}

std::vector<RoomId> editRooms(const WorldPlan& plan, std::span<const EditOperation> operations) {
    std::set<RoomId> named;
    const auto gameRoom = [&](const GameId& game) {
        for (const Placement& placement : plan.placements) {
            if (placement.game == game) {
                named.insert(placement.slot.fixture().room());
            }
        }
    };
    for (const EditOperation& operation : operations) {
        if (operation.game.isValid()) {
            gameRoom(operation.game);
        }
        if (operation.with.isValid()) {
            gameRoom(operation.with);
        }
        if (!operation.slot.empty()) {
            named.insert(operation.slot.fixture().room());
        }
        if (!operation.fixture.empty()) {
            named.insert(operation.fixture.room());
        }
    }
    std::set<std::string> floors;
    for (const Room& room : plan.rooms) {
        if (named.contains(room.id)) {
            floors.insert(room.floor);
        }
    }
    std::vector<RoomId> rooms;
    for (const Room& room : plan.rooms) {
        if (floors.contains(room.floor)) {
            rooms.push_back(room.id);
        }
    }
    std::sort(rooms.begin(), rooms.end());
    return rooms;
}

Expected<GenerationResult> editedResult(const WorldPlan& stored, std::string_view storedDigest,
                                        const EditedWorld& edited, std::span<const EditResult> results) {
    GenerationResult result;
    result.plan = edited.plan;
    sortPlan(result.plan);
    ChangeReport& changes = result.changes;
    changes.outcome = GenerationOutcome::Updated;
    changes.rebuild = RebuildLevel::NoRebuild;
    std::map<std::string, const ValidationReport*> floors;
    for (const EditResult& edit : results) {
        if (edit.refused || !edit.changed) {
            continue;
        }
        changes.rebuild = std::max(changes.rebuild, edit.rebuild);
        changes.fixturesMoved.insert(changes.fixturesMoved.end(), edit.fixturesMoved.begin(), edit.fixturesMoved.end());
        if (!edit.floor.empty()) {
            floors[edit.floor] = &edit.validation;
        }
    }
    std::sort(changes.fixturesMoved.begin(), changes.fixturesMoved.end());
    changes.fixturesMoved.erase(std::unique(changes.fixturesMoved.begin(), changes.fixturesMoved.end()),
                                changes.fixturesMoved.end());
    WorldPlan before = stored;
    sortPlan(before);
    std::vector<Placement> kept;
    std::set_intersection(before.placements.begin(), before.placements.end(), result.plan.placements.begin(),
                          result.plan.placements.end(), std::back_inserter(kept));
    changes.kept = kept.size();
    changes.digestBefore = std::string(storedDigest);
    auto after = planDigest(result.plan);
    if (!after) {
        return propagate(after.error());
    }
    changes.digestAfter = std::move(*after);
    for (const auto& [floor, validation] : floors) {
        result.validation.findings.insert(result.validation.findings.end(), validation->findings.begin(),
                                          validation->findings.end());
    }
    return result;
}

Expected<std::vector<EditOperation>> parseEditScript(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    detail::DocumentReader document(text, std::string(context));
    const detail::ObjectReader root(document, *value, "", {"operations", "schemaVersion"});
    const std::int64_t schema = root.integer("schemaVersion", 1, std::numeric_limits<std::int64_t>::max());
    if (!document.failed() && schema > kEditScriptSchemaVersion) {
        document.fail("/schemaVersion",
                      fmt::format("the script is of schema {}; this Retroverse reads {}", schema,
                                  kEditScriptSchemaVersion),
                      ErrorCode::Unsupported);
    }
    std::vector<EditOperation> operations;
    const Json& list = root.array("operations");
    detail::forEachElement(list, root.pointer("operations"), [&](const Json& element, const std::string& pointer) {
        if (document.failed()) {
            return;
        }
        if (!element.is_object() || !element.contains("op") || !element["op"].is_string()) {
            document.fail(pointer, "an operation is an object with its \"op\"");
            return;
        }
        const auto kind = parseEditKind(element["op"].get<std::string>());
        if (!kind) {
            document.fail(pointer + "/op",
                          "expected one of move, swap, pin, unpin, move-fixture, pin-fixture, unpin-fixture");
            return;
        }
        EditOperation operation;
        operation.kind = *kind;
        const auto game = [](std::string_view id) { return GameId::parse(id); };
        switch (*kind) {
        case EditKind::Move: {
            const detail::ObjectReader reader(document, element, pointer, {"game", "op", "slot"});
            operation.game = reader.parsed<GameId>("game", game, "a GameId");
            operation.slot = reader.parsed<SlotId>("slot", SlotId::parse, "a slot id");
            break;
        }
        case EditKind::Swap: {
            const detail::ObjectReader reader(document, element, pointer, {"game", "op", "with"});
            operation.game = reader.parsed<GameId>("game", game, "a GameId");
            operation.with = reader.parsed<GameId>("with", game, "a GameId");
            break;
        }
        case EditKind::Pin:
        case EditKind::Unpin: {
            const detail::ObjectReader reader(document, element, pointer, {"game", "op"});
            operation.game = reader.parsed<GameId>("game", game, "a GameId");
            break;
        }
        case EditKind::MoveFixture: {
            const detail::ObjectReader reader(document, element, pointer, {"fixture", "op", "xMm", "yawCdeg", "zMm"});
            operation.fixture = reader.parsed<FixtureId>("fixture", FixtureId::parse, "a fixture id");
            operation.xMm = reader.integer("xMm", -kMaxFixtureOffsetMm, kMaxFixtureOffsetMm);
            operation.zMm = reader.integer("zMm", -kMaxFixtureOffsetMm, kMaxFixtureOffsetMm);
            if (const auto yaw = reader.optionalInteger("yawCdeg", 0, 35999)) {
                operation.yawCdeg = static_cast<std::int32_t>(*yaw);
            }
            break;
        }
        case EditKind::PinFixture:
        case EditKind::UnpinFixture: {
            const detail::ObjectReader reader(document, element, pointer, {"fixture", "op"});
            operation.fixture = reader.parsed<FixtureId>("fixture", FixtureId::parse, "a fixture id");
            break;
        }
        }
        operations.push_back(std::move(operation));
    });
    if (document.failed()) {
        return propagate(document.error());
    }
    return operations;
}

std::string writeEditScript(std::span<const EditOperation> operations) {
    Json list = Json::array();
    for (const EditOperation& operation : operations) {
        list.push_back(operationJson(operation));
    }
    const Json document{{"schemaVersion", kEditScriptSchemaVersion}, {"operations", std::move(list)}};
    return document.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

} // namespace rv::world
