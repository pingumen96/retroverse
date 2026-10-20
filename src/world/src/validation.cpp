#include <rv/world/validation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <rv/assets/prefab_manifest.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/world/nav_grid.hpp>

#include "layout_score.hpp"

namespace rv::world {
namespace {

using catalog::GameId;

constexpr std::array<ValidationRule, 34> kRules{{
    {rule::kRoomUnknown, Severity::Hard,
     "Every fixture and unplaced entry names a room the plan lists (a console room's games unplaced for capacity may "
     "name the room its full floor did not take)."},
    {rule::kGroundFloorMissing, Severity::Hard, "The plan holds the ground floor."},
    {rule::kFloorUnknown, Severity::Hard,
     "Every room stands on a floor of the plan, and every floor of the plan is the building's, at its order."},
    {rule::kRoomsOverlap, Severity::Hard, "No two rooms' footprints on one floor overlap."},
    {rule::kSystemRooms, Severity::Hard,
     "Every system stands in exactly one room or one section; the arcade room stands on the ground floor."},
    {rule::kRoomTemplate, Severity::Hard, "Every room's template is known, at the version the room was made from."},
    {rule::kRoomSize, Severity::Hard,
     "Every room has its template's interior and height at its growth step, within the template's growth limit."},
    {rule::kFixtureSocket, Severity::Hard,
     "Every fixture stands in a socket of its room's template that exists at the room's growth step and takes its "
     "role."},
    {rule::kFixtureLevels, Severity::Hard,
     "A fixture's levels are its role's: featured on displays, band or spine on hero-wall modules (band only in the "
     "template's hero band), spine on side-wall modules, none on other fixtures; each level once, with slots."},
    {rule::kPrefabSlots, Severity::Hard,
     "The levels of every collection fixture equal its prefab's Slot_<level>_<index> sockets."},
    {rule::kFixtureOutsideRoom, Severity::Hard, "Every fixture lies inside its room, below the ceiling."},
    {rule::kDoorReserved, Severity::Hard, "No fixture stands in or enters a door-reserved socket."},
    {rule::kDoorwayClearance, Severity::Hard, "No fixture enters the doorway's clear area."},
    {rule::kFootprintsOverlap, Severity::Hard, "No two fixtures' boxes overlap (floor coverings excepted)."},
    {rule::kLightsOverLimit, Severity::Hard, "No room holds more point and spot lights than its template allows."},
    {rule::kSlotUnknown, Severity::Hard,
     "Every placement names a slot of a fixture of the plan: a listed level and an index below its slot count."},
    {rule::kSlotShared, Severity::Hard, "No slot holds two games."},
    {rule::kDressingUnknown, Severity::Hard,
     "Every dressed span lies within a listed level of a fixture of the plan and covers at least one slot."},
    {rule::kDressingOverlap, Severity::Hard, "No two dressed spans of one level overlap."},
    {rule::kSlotDressed, Severity::Hard, "No placement lies in a dressed span."},
    {rule::kGameTwice, Severity::Hard, "No game has more than one placement or unplaced entry."},
    {rule::kGameMissing, Severity::Hard,
     "Every game of a room's selection has a placement or an unplaced entry (with a selection only)."},
    {rule::kRepresentativeUnknown, Severity::Hard,
     "The representative of every duplicate title is another game the plan places or lists for another reason."},
    {rule::kPinBroken, Severity::Hard, "Every pin that still names something in the plan is honoured."},
    {rule::kHeroTvMissing, Severity::Hard, "Every room holds exactly one hero TV, its hero corner's crt."},
    {rule::kDoorwayBlocked, Severity::Hard,
     "The doorway's clear area leaves a body of the minimum clear width room to stand in."},
    {rule::kUnreachable, Severity::Hard,
     "Every module, display fixture and the hero TV can be walked to from the doorway through the minimum clear "
     "width."},
    {rule::kRoomKept, Severity::Soft,
     "A room an update kept as it is (an older template or prefab, another generator major) is reported."},
    {rule::kPrefabUnresolved, Severity::Soft, "Every collection fixture's prefab can be resolved and its slots read."},
    {rule::kPinOrphaned, Severity::Soft, "No pin names a game, slot, fixture or socket that no longer exists."},
    {rule::kTvLineOfSight, Severity::Soft, "The hero TV can be seen from the doorway."},
    {rule::kPassageNarrow, Severity::Soft,
     "Every target reachable through the minimum clear width is also reachable through the main passage width."},
    {rule::kSocketBare, Severity::Soft, "Every hero-wall and side-wall socket holds a module or decoration."},
    {rule::kDecorationDensity, Severity::Soft,
     "The share of decoration sockets in use falls in the configured decoration density's band (style §6.3)."},
}};

std::size_t ruleIndex(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kRules.size(); ++i) {
        if (kRules[i].name == name) {
            return i;
        }
    }
    return kRules.size();
}

/// The findings of a run, put in the report's order when finished.
class Findings {
public:
    /// `ruleName` is one of kRules' names (the rule:: constants).
    void add(std::string_view ruleName, std::string subject, std::string message) {
        const std::size_t index = std::min(ruleIndex(ruleName), kRules.size() - 1);
        m_entries.push_back({index, ValidationFinding{std::string(ruleName), kRules[index].severity, std::move(subject),
                                                      std::move(message)}});
    }

    [[nodiscard]] std::size_t count(std::string_view ruleName) const noexcept {
        const std::size_t index = ruleIndex(ruleName);
        return static_cast<std::size_t>(std::count_if(m_entries.begin(), m_entries.end(),
                                                      [index](const Entry& entry) { return entry.rule == index; }));
    }

    /// Rule by rule in the table's order, each rule's findings by subject then message, a finding repeated once.
    [[nodiscard]] std::vector<ValidationFinding> finish() && {
        std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
            return std::tie(a.rule, a.finding.subject, a.finding.message) <
                   std::tie(b.rule, b.finding.subject, b.finding.message);
        });
        std::vector<ValidationFinding> findings;
        findings.reserve(m_entries.size());
        for (Entry& entry : m_entries) {
            if (findings.empty() || !(findings.back() == entry.finding)) {
                findings.push_back(std::move(entry.finding));
            }
        }
        return findings;
    }

private:
    struct Entry {
        std::size_t rule = 0;
        ValidationFinding finding;
    };
    std::vector<Entry> m_entries;
};

// --- Geometry ----------------------------------------------------------------------------------------------------

/// The sine and cosine of a yaw in hundredths of a degree, from Taylor series on a quarter turn (basic arithmetic
/// only, so every build computes the same values; the error stays below 1e-13), exact at the quarter turns.
struct Direction {
    double sin = 0.0;
    double cos = 1.0;
};

Direction directionOf(std::int32_t yawCdeg) noexcept {
    const std::int32_t yaw = ((yawCdeg % 36'000) + 36'000) % 36'000;
    const std::int32_t quadrant = yaw / 9'000;
    const std::int32_t rest = yaw % 9'000;
    constexpr double kPi = 3.14159265358979323846;
    const double x = static_cast<double>(rest) * (kPi / 18'000.0);
    const double x2 = x * x;
    double sine = 0.0;
    double cosine = 0.0;
    double sineTerm = x;
    double cosineTerm = 1.0;
    for (int n = 0; n <= 16; n += 2) {
        cosine += cosineTerm;
        sine += sineTerm;
        cosineTerm *= -x2 / static_cast<double>((n + 1) * (n + 2));
        sineTerm *= -x2 / static_cast<double>((n + 2) * (n + 3));
    }
    switch (quadrant) {
    case 1:
        return {cosine, -sine};
    case 2:
        return {-sine, -cosine};
    case 3:
        return {-cosine, sine};
    default:
        return {sine, cosine};
    }
}

/// An axis-aligned box in half millimetres (a fixture of odd size centred on its pivot keeps exact edges). Boxes that
/// touch do not overlap.
struct Box {
    std::int64_t minX = 0;
    std::int64_t maxX = 0;
    std::int64_t minY = 0;
    std::int64_t maxY = 0;
    std::int64_t minZ = 0;
    std::int64_t maxZ = 0;

    [[nodiscard]] bool overlaps(const Box& other) const noexcept {
        return minX < other.maxX && other.minX < maxX && minY < other.maxY && other.minY < maxY && minZ < other.maxZ &&
               other.minZ < maxZ;
    }
    /// The floors' rectangles overlap (heights not compared).
    [[nodiscard]] bool overlapsOnFloor(const Box& other) const noexcept {
        return minX < other.maxX && other.minX < maxX && minZ < other.maxZ && other.minZ < maxZ;
    }
};

/// Where a prefab's origin lies in its bounds (rv/geometry/prefabs.hpp): most kinds stand on it ("floor": the middle
/// of the footprint at the bottom), a poster frame hangs from it (the middle of its back), a cartridge is centred on
/// it.
enum class Pivot : std::uint8_t {
    Floor,
    BackCentre,
    Centre,
};

Pivot pivotOf(const PrefabRef& prefab) noexcept {
    if (prefab.kind == "poster_frame") {
        return Pivot::BackCentre;
    }
    return prefab.kind == "cartridge" ? Pivot::Centre : Pivot::Floor;
}

/// The middle of a fixture's footprint in half millimetres: its pivot, or for a prefab hung from the middle of its back
/// that point moved half its depth out along its front.
std::pair<std::int64_t, std::int64_t> footprintCentreOf(const Fixture& fixture) noexcept {
    std::int64_t x = 2 * fixture.xMm;
    std::int64_t z = 2 * fixture.zMm;
    if (pivotOf(fixture.prefab) == Pivot::BackCentre) {
        const Direction front = directionOf(fixture.yawCdeg);
        x += std::llround(front.sin * static_cast<double>(fixture.depthMm));
        z += std::llround(front.cos * static_cast<double>(fixture.depthMm));
    }
    return {x, z};
}

/// A fixture's box: its bounds turned by its yaw about its pivot, rounded outwards to half millimetres.
Box boxOf(const Fixture& fixture) noexcept {
    // The half extents in half millimetres are the full extents in millimetres.
    std::int64_t extentX = 0;
    std::int64_t extentZ = 0;
    const std::int32_t yaw = ((fixture.yawCdeg % 36'000) + 36'000) % 36'000;
    if (yaw % 9'000 == 0) {
        const bool quarterTurn = (yaw / 9'000) % 2 == 1;
        extentX = quarterTurn ? fixture.depthMm : fixture.widthMm;
        extentZ = quarterTurn ? fixture.widthMm : fixture.depthMm;
    } else {
        const Direction direction = directionOf(yaw);
        const double width = static_cast<double>(fixture.widthMm);
        const double depth = static_cast<double>(fixture.depthMm);
        // One more half millimetre covers the rounding of a hung prefab's centre.
        const std::int64_t margin = pivotOf(fixture.prefab) == Pivot::BackCentre ? 1 : 0;
        extentX =
            static_cast<std::int64_t>(std::ceil(std::abs(width * direction.cos) + std::abs(depth * direction.sin))) +
            margin;
        extentZ =
            static_cast<std::int64_t>(std::ceil(std::abs(width * direction.sin) + std::abs(depth * direction.cos))) +
            margin;
    }
    const auto [centreX, centreZ] = footprintCentreOf(fixture);
    const std::int64_t bottom =
        pivotOf(fixture.prefab) == Pivot::Floor ? 2 * fixture.yMm : 2 * fixture.yMm - fixture.heightMm;
    return Box{centreX - extentX, centreX + extentX, bottom, bottom + 2 * fixture.heightMm,
               centreZ - extentZ, centreZ + extentZ};
}

/// A rectangle of the floor (millimetres) as a box in half millimetres from `minYMm` to `maxYMm`.
Box boxOf(const Footprint& footprint, std::int64_t minYMm, std::int64_t maxYMm) noexcept {
    return Box{2 * footprint.minXMm, 2 * footprint.maxXMm, 2 * minYMm,
               2 * maxYMm,           2 * footprint.minZMm, 2 * footprint.maxZMm};
}

std::int64_t floorHalf(std::int64_t halfMm) noexcept {
    return (halfMm >= 0 || halfMm % 2 == 0) ? halfMm / 2 : halfMm / 2 - 1;
}
std::int64_t ceilHalf(std::int64_t halfMm) noexcept {
    return (halfMm <= 0 || halfMm % 2 == 0) ? halfMm / 2 : halfMm / 2 + 1;
}

/// A box's floor in whole millimetres, rounded outwards (the navigation grid's obstacles).
Footprint floorOf(const Box& box) noexcept {
    return Footprint{floorHalf(box.minX), floorHalf(box.minZ), ceilHalf(box.maxX), ceilHalf(box.maxZ)};
}

/// Half millimetres as millimetres: "-4295" or "-4295.5".
std::string mmText(std::int64_t halfMm) {
    if (halfMm % 2 == 0) {
        return std::to_string(halfMm / 2);
    }
    return fmt::format("{}{}.5", halfMm < 0 ? "-" : "", std::abs(halfMm) / 2);
}

std::string floorText(const Box& box) {
    return fmt::format("x {} to {}, z {} to {}", mmText(box.minX), mmText(box.maxX), mmText(box.minZ),
                       mmText(box.maxZ));
}

std::string footprintText(const Footprint& footprint) {
    return fmt::format("x {} to {}, z {} to {}", footprint.minXMm, footprint.maxXMm, footprint.minZMm,
                       footprint.maxZMm);
}

bool isFloorCovering(const Fixture& fixture) noexcept {
    return fixture.yMm == 0 && fixture.heightMm <= kFloorCoveringMaxHeightMm;
}

/// Whether the segment from `from` to `to` (half millimetres) passes through the inside of `box` (touching a face is
/// not crossing it).
bool segmentCrosses(const std::array<double, 3>& from, const std::array<double, 3>& to, const Box& box) noexcept {
    const std::array<double, 3> low{static_cast<double>(box.minX), static_cast<double>(box.minY),
                                    static_cast<double>(box.minZ)};
    const std::array<double, 3> high{static_cast<double>(box.maxX), static_cast<double>(box.maxY),
                                     static_cast<double>(box.maxZ)};
    double enter = 0.0;
    double leave = 1.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double delta = to[axis] - from[axis];
        if (delta == 0.0) {
            if (from[axis] <= low[axis] || from[axis] >= high[axis]) {
                return false;
            }
            continue;
        }
        double t0 = (low[axis] - from[axis]) / delta;
        double t1 = (high[axis] - from[axis]) / delta;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
        if (enter >= leave) {
            return false;
        }
    }
    return true;
}

// --- The plan's indexes ------------------------------------------------------------------------------------------

/// A room with its template (null when it is not known or of another version) and its fixtures by id.
struct RoomView {
    const Room* room = nullptr;
    const RoomTemplate* roomTemplate = nullptr;
    std::vector<const Fixture*> fixtures;
};

const FixtureLevel* levelOf(const Fixture& fixture, std::uint32_t level) noexcept {
    for (const FixtureLevel& candidate : fixture.levels) {
        if (candidate.level == level) {
            return &candidate;
        }
    }
    return nullptr;
}

/// Where a slot id leads in the plan.
enum class SlotLookup : std::uint8_t {
    Exists,
    NoFixture,
    NoLevel,
    OutOfRange,
};

SlotLookup lookUpSlot(const std::map<FixtureId, const Fixture*>& fixtures, const SlotId& slot) {
    const auto found = fixtures.find(slot.fixture());
    if (found == fixtures.end()) {
        return SlotLookup::NoFixture;
    }
    const FixtureLevel* level = levelOf(*found->second, slot.level());
    if (level == nullptr) {
        return SlotLookup::NoLevel;
    }
    return slot.index() < level->slots ? SlotLookup::Exists : SlotLookup::OutOfRange;
}

/// The roles that hold games, whose prefab's slots are compared and which must be reachable.
bool isCollectionFixture(FixtureRole role) noexcept {
    return role == FixtureRole::HeroModule || role == FixtureRole::SideModule || role == FixtureRole::Display;
}

bool isHeroTv(const Fixture& fixture) noexcept {
    return fixture.role == FixtureRole::HeroCorner && fixture.id.part() == kHeroTvPart;
}

/// The socket kinds a role may stand in.
bool roleFitsSocket(FixtureRole role, SocketKind kind) noexcept {
    switch (role) {
    case FixtureRole::HeroModule:
        return kind == SocketKind::HeroModule;
    case FixtureRole::SideModule:
        return kind == SocketKind::SideWall;
    case FixtureRole::Display:
        return kind == SocketKind::Display;
    case FixtureRole::HeroCorner:
        return kind == SocketKind::HeroCorner;
    case FixtureRole::Decoration:
        return kind == SocketKind::SideWall || kind == SocketKind::Decoration;
    }
    return false;
}

std::string socketIdText(const Room& room, std::string_view socket) {
    const auto id = FixtureId::make(room.id, socket);
    return id ? id->toString() : room.id.toString() + "/fx:" + std::string(socket);
}

// --- The validation ----------------------------------------------------------------------------------------------

class Validator {
public:
    Validator(const WorldPlan& plan, const ValidationContext& context) : m_plan(plan), m_context(context) {}

    ValidationReport run() {
        indexPlan();
        checkBuilding();
        checkRoomTemplates();
        checkFixtureSockets();
        checkFixtureLevels();
        checkPrefabSlots();
        checkGeometry();
        checkLights();
        checkSlots();
        checkDressing();
        checkGames();
        checkPins();
        checkNavigation();
        checkSockets();

        ValidationReport report;
        report.layoutScorePermille = layoutScore();
        report.findings = std::move(m_findings).finish();
        return report;
    }

private:
    void indexPlan() {
        for (const Room& room : m_plan.rooms) {
            m_rooms.try_emplace(room.id, RoomView{&room, nullptr, {}});
        }
        std::vector<const Fixture*> fixtures;
        for (const Fixture& fixture : m_plan.fixtures) {
            fixtures.push_back(&fixture);
        }
        std::stable_sort(fixtures.begin(), fixtures.end(),
                         [](const Fixture* a, const Fixture* b) { return a->id < b->id; });
        for (const Fixture* fixture : fixtures) {
            if (!m_fixtures.try_emplace(fixture->id, fixture).second) {
                m_findings.add(rule::kFixtureSocket, fixture->id.toString(), "the plan lists the fixture twice");
                continue;
            }
            const auto room = m_rooms.find(fixture->id.room());
            if (room == m_rooms.end()) {
                m_findings.add(rule::kRoomUnknown, fixture->id.toString(),
                               fmt::format("the fixture stands in {}, which the plan does not list",
                                           fixture->id.room().toString()));
                continue;
            }
            room->second.fixtures.push_back(fixture);
        }
        for (const UnplacedGame& unplaced : m_plan.unplaced) {
            const bool notTaken = unplaced.reason == UnplacedReason::Capacity &&
                                  unplaced.room.kind() == RoomIdKind::System && !unplaced.room.isExpansion();
            if (!m_rooms.contains(unplaced.room) && !notTaken) {
                m_findings.add(rule::kRoomUnknown, unplaced.game.toString(),
                               fmt::format("the game is listed as unplaced in {}, which the plan does not list",
                                           unplaced.room.toString()));
            }
        }
        for (const Placement& placement : m_plan.placements) {
            ++m_entries[placement.game].placements;
        }
        for (const UnplacedGame& unplaced : m_plan.unplaced) {
            Entries& entries = m_entries[unplaced.game];
            ++entries.unplaced;
            if (unplaced.reason != UnplacedReason::DuplicateTitle) {
                ++entries.unplacedOtherThanDuplicate;
            }
        }
    }

    [[nodiscard]] bool isKept(const RoomId& room) const noexcept {
        return std::any_of(m_context.keptRooms.begin(), m_context.keptRooms.end(),
                           [&](const KeptRoom& kept) { return kept.room == room; });
    }

    /// The building's rules that need no hall (ADR-0013 §11).
    void checkBuilding() {
        for (const KeptRoom& kept : m_context.keptRooms) {
            m_findings.add(rule::kRoomKept, kept.room.toString(),
                           fmt::format("the room is kept as it is until a regeneration: {}", kept.reason));
        }
        std::map<std::string, std::uint32_t> floors;
        for (const Floor& floor : m_plan.floors) {
            floors.emplace(floor.id, floor.order);
            if (m_context.building) {
                const auto order = m_context.building->order(floor.id);
                if (!order) {
                    m_findings.add(rule::kFloorUnknown, floor.id, "the building has no such floor");
                } else if (*order != floor.order) {
                    m_findings.add(rule::kFloorUnknown, floor.id,
                                   fmt::format("the floor's order is {}, the building's {}", floor.order, *order));
                }
            }
        }
        if (!floors.contains(std::string(kGroundFloor))) {
            m_findings.add(rule::kGroundFloorMissing, "", "the plan does not hold the ground floor");
        }
        const BuildingLayout layout = m_context.building ? m_context.building->layout : BuildingLayout{0, 0, 0, 0, 0};
        std::map<std::string, std::vector<const Room*>> byFloor;
        std::map<catalog::SystemName, std::vector<std::string>> standsIn;
        for (const Room& room : m_plan.rooms) {
            if (!floors.contains(room.floor)) {
                m_findings.add(rule::kFloorUnknown, room.id.toString(),
                               fmt::format("the room stands on the floor '{}', which the plan does not list",
                                           room.floor));
            }
            if (room.kind == RoomKind::Arcade && room.floor != kGroundFloor) {
                m_findings.add(rule::kSystemRooms, room.id.toString(),
                               fmt::format("the arcade room stands on the floor '{}', not on the ground floor",
                                           room.floor));
            }
            byFloor[room.floor].push_back(&room);
            for (const catalog::SystemName& system : room.sections) {
                standsIn[system].push_back(room.id.toString());
            }
        }
        for (auto& [system, rooms] : standsIn) {
            std::sort(rooms.begin(), rooms.end());
            if (rooms.size() > 1) {
                m_findings.add(rule::kSystemRooms, system.str(),
                               fmt::format("the system stands in {} rooms or sections: {}", rooms.size(),
                                           fmt::join(rooms, ", ")));
            }
        }
        for (auto& [floor, rooms] : byFloor) {
            std::sort(rooms.begin(), rooms.end(), [](const Room* a, const Room* b) { return a->id < b->id; });
            for (std::size_t i = 0; i < rooms.size(); ++i) {
                const Footprint a = floorFootprint(*rooms[i], layout);
                for (std::size_t j = i + 1; j < rooms.size(); ++j) {
                    const Footprint b = floorFootprint(*rooms[j], layout);
                    if (a.overlaps(b)) {
                        m_findings.add(rule::kRoomsOverlap, rooms[i]->id.toString(),
                                       fmt::format("its footprint on the floor '{}', {}, overlaps {}'s, {}", floor,
                                                   footprintText(a), rooms[j]->id.toString(), footprintText(b)));
                    }
                }
            }
        }
    }

    void checkRoomTemplates() {
        for (auto& [id, view] : m_rooms) {
            const Room& room = *view.room;
            const auto found =
                std::find_if(m_context.templates.begin(), m_context.templates.end(),
                             [&](const RoomTemplate& candidate) { return candidate.id == room.templateId; });
            if (found == m_context.templates.end()) {
                m_findings.add(rule::kRoomTemplate, id.toString(),
                               fmt::format("the room's template '{}' is not known", room.templateId));
                continue;
            }
            if (found->version != room.templateVersion && isKept(id)) {
                continue; // kept as an older template made it: its geometry is not judged against the new one
            }
            if (found->version != room.templateVersion) {
                m_findings.add(rule::kRoomTemplate, id.toString(),
                               fmt::format("the room was made from version {} of the template '{}', which is now "
                                           "version {}",
                                           room.templateVersion, room.templateId, found->version));
                continue;
            }
            view.roomTemplate = &*found;
            const RoomTemplate& roomTemplate = *found;
            if (room.growthSteps > roomTemplate.maxGrowthSteps()) {
                m_findings.add(rule::kRoomSize, id.toString(),
                               fmt::format("the room took {} growth steps; its template allows {}", room.growthSteps,
                                           roomTemplate.maxGrowthSteps()));
            }
            const Footprint expected = interiorFootprint(roomTemplate, room.growthSteps);
            const Footprint actual{room.minXMm, room.minZMm, room.maxXMm, room.maxZMm};
            if (!(actual == expected) || room.heightMm != roomTemplate.heightMm) {
                m_findings.add(rule::kRoomSize, id.toString(),
                               fmt::format("the room's interior is {}, {} high; its template's after {} growth steps "
                                           "is {}, {} high",
                                           footprintText(actual), room.heightMm, room.growthSteps,
                                           footprintText(expected), roomTemplate.heightMm));
            }
        }
    }

    /// The template socket a fixture stands in, if its room has a template that holds the socket at its growth step.
    [[nodiscard]] const TemplateSocket* socketOf(const RoomView& view, const Fixture& fixture) const noexcept {
        if (view.roomTemplate == nullptr) {
            return nullptr;
        }
        const TemplateSocket* socket = view.roomTemplate->socket(fixture.id.socket());
        if (socket == nullptr || socket->growthStep > view.room->growthSteps) {
            return nullptr;
        }
        return socket;
    }

    void checkFixtureSockets() {
        for (const auto& [id, view] : m_rooms) {
            if (view.roomTemplate == nullptr) {
                continue;
            }
            for (const Fixture* fixture : view.fixtures) {
                const std::string subject = fixture->id.toString();
                const TemplateSocket* templateSocket = view.roomTemplate->socket(fixture->id.socket());
                if (templateSocket == nullptr) {
                    m_findings.add(rule::kFixtureSocket, subject,
                                   fmt::format("the template '{}' has no socket '{}'", view.roomTemplate->id,
                                               fixture->id.socket()));
                    continue;
                }
                if (templateSocket->growthStep > view.room->growthSteps) {
                    m_findings.add(rule::kFixtureSocket, subject,
                                   fmt::format("the socket '{}' comes with growth step {}; the room took {}",
                                               templateSocket->id, templateSocket->growthStep, view.room->growthSteps));
                    continue;
                }
                if (templateSocket->kind == SocketKind::DoorReserved) {
                    continue; // door-reserved
                }
                if (!roleFitsSocket(fixture->role, templateSocket->kind)) {
                    m_findings.add(rule::kFixtureSocket, subject,
                                   fmt::format("a {} fixture cannot stand in the {} socket '{}'",
                                               toString(fixture->role), toString(templateSocket->kind),
                                               templateSocket->id));
                    continue;
                }
                const std::string& part = fixture->id.part();
                if (fixture->role == FixtureRole::HeroCorner) {
                    const bool known = std::any_of(templateSocket->parts.begin(), templateSocket->parts.end(),
                                                   [&](const HeroPart& heroPart) { return heroPart.role == part; });
                    if (!known) {
                        m_findings.add(rule::kFixtureSocket, subject,
                                       part.empty() ? fmt::format("a hero corner fixture names one of the parts of "
                                                                  "'{}'",
                                                                  templateSocket->id)
                                                    : fmt::format("the hero corner '{}' has no part '{}'",
                                                                  templateSocket->id, part));
                    }
                } else if (fixture->role != FixtureRole::Decoration && !part.empty()) {
                    m_findings.add(rule::kFixtureSocket, subject,
                                   fmt::format("a {} fixture is its socket's only fixture and names no part",
                                               toString(fixture->role)));
                }
            }
        }
    }

    void checkFixtureLevels() {
        for (const auto& [id, fixture] : m_fixtures) {
            const std::string subject = id.toString();
            std::set<std::uint32_t> seen;
            const auto room = m_rooms.find(id.room());
            const RoomTemplate* roomTemplate = room == m_rooms.end() ? nullptr : room->second.roomTemplate;
            const bool inHeroBand =
                roomTemplate != nullptr && std::find(roomTemplate->heroBand.begin(), roomTemplate->heroBand.end(),
                                                     id.socket()) != roomTemplate->heroBand.end();
            if (!isCollectionFixture(fixture->role) && !fixture->levels.empty()) {
                m_findings.add(rule::kFixtureLevels, subject,
                               fmt::format("a {} fixture holds no slots, but the plan lists {} levels",
                                           toString(fixture->role), fixture->levels.size()));
                continue;
            }
            for (const FixtureLevel& level : fixture->levels) {
                if (!seen.insert(level.level).second) {
                    m_findings.add(rule::kFixtureLevels, subject, fmt::format("level {} is listed twice", level.level));
                }
                if (level.slots == 0) {
                    m_findings.add(rule::kFixtureLevels, subject, fmt::format("level {} has no slots", level.level));
                }
                bool fits = false;
                switch (fixture->role) {
                case FixtureRole::HeroModule:
                    fits = level.section == Section::Spine ||
                           (level.section == Section::Band && (roomTemplate == nullptr || inHeroBand));
                    break;
                case FixtureRole::SideModule:
                    fits = level.section == Section::Spine;
                    break;
                case FixtureRole::Display:
                    fits = level.section == Section::Featured;
                    break;
                case FixtureRole::HeroCorner:
                case FixtureRole::Decoration:
                    break;
                }
                if (!fits) {
                    m_findings.add(
                        rule::kFixtureLevels, subject,
                        fmt::format("level {} is a {} level, which a {} fixture{} does not hold", level.level,
                                    toString(level.section), toString(fixture->role),
                                    fixture->role == FixtureRole::HeroModule && level.section == Section::Band
                                        ? " outside the template's hero band"
                                        : ""));
                }
            }
        }
    }

    void checkPrefabSlots() {
        if (!m_context.prefabSlots) {
            return;
        }
        std::map<PrefabRef, Expected<std::vector<PrefabSlotLevel>>> resolved;
        for (const auto& [id, fixture] : m_fixtures) {
            if (!isCollectionFixture(fixture->role) || isKept(id.room())) {
                continue;
            }
            auto found = resolved.find(fixture->prefab);
            if (found == resolved.end()) {
                Expected<std::vector<PrefabSlotLevel>> slots = m_context.prefabSlots(fixture->prefab);
                if (slots) {
                    std::sort(slots->begin(), slots->end());
                }
                found = resolved.emplace(fixture->prefab, std::move(slots)).first;
            }
            const std::string subject = id.toString();
            if (!found->second) {
                m_findings.add(rule::kPrefabUnresolved, subject,
                               fmt::format("the slots of the prefab {} ({}) cannot be read: {}",
                                           fixture->prefab.manifest, fixture->prefab.kind,
                                           found->second.error().toString()));
                continue;
            }
            std::vector<PrefabSlotLevel> listed;
            for (const FixtureLevel& level : fixture->levels) {
                listed.push_back({level.level, level.slots});
            }
            std::sort(listed.begin(), listed.end());
            if (listed != *found->second) {
                const auto describe = [](const std::vector<PrefabSlotLevel>& levels) {
                    std::string text;
                    for (const PrefabSlotLevel& level : levels) {
                        text += fmt::format("{}{}: {}", text.empty() ? "" : ", ", level.level, level.slots);
                    }
                    return text.empty() ? std::string("none") : text;
                };
                m_findings.add(rule::kPrefabSlots, subject,
                               fmt::format("the plan lists the slots per level {}; the prefab {} has {}",
                                           describe(listed), fixture->prefab.manifest, describe(*found->second)));
            }
        }
    }

    void checkGeometry() {
        for (const auto& [id, view] : m_rooms) {
            const Room& room = *view.room;
            const Box interior = boxOf(Footprint{room.minXMm, room.minZMm, room.maxXMm, room.maxZMm}, 0, room.heightMm);
            for (const Fixture* fixture : view.fixtures) {
                const Box box = boxOf(*fixture);
                if (box.minX < interior.minX || box.maxX > interior.maxX || box.minZ < interior.minZ ||
                    box.maxZ > interior.maxZ || box.minY < interior.minY || box.maxY > interior.maxY) {
                    m_findings.add(rule::kFixtureOutsideRoom, fixture->id.toString(),
                                   fmt::format("the fixture covers {}, from y {} to {}; the room's interior is {}, {} "
                                               "high",
                                               floorText(box), mmText(box.minY), mmText(box.maxY), floorText(interior),
                                               room.heightMm));
                }
            }
            if (view.roomTemplate != nullptr) {
                const RoomTemplate& roomTemplate = *view.roomTemplate;
                for (const Fixture* fixture : view.fixtures) {
                    const Box box = boxOf(*fixture);
                    const std::string subject = fixture->id.toString();
                    for (const TemplateSocket& socket : roomTemplate.sockets) {
                        if (socket.kind != SocketKind::DoorReserved || socket.growthStep > room.growthSteps) {
                            continue;
                        }
                        if (socket.id == fixture->id.socket()) {
                            m_findings.add(
                                rule::kDoorReserved, subject,
                                fmt::format("the fixture stands in the door-reserved socket '{}'", socket.id));
                            continue;
                        }
                        const Footprint reserved = socketFootprint(roomTemplate, socket, room.growthSteps);
                        if (box.overlapsOnFloor(boxOf(reserved, 0, 0))) {
                            m_findings.add(rule::kDoorReserved, subject,
                                           fmt::format("the fixture covers {}, entering the door-reserved socket '{}' "
                                                       "({})",
                                                       floorText(box), socket.id, footprintText(reserved)));
                        }
                    }
                    const Footprint clearance = doorwayClearance(roomTemplate, room.growthSteps);
                    if (box.overlapsOnFloor(boxOf(clearance, 0, 0))) {
                        m_findings.add(rule::kDoorwayClearance, subject,
                                       fmt::format("the fixture covers {}, entering the doorway's clear area ({})",
                                                   floorText(box), footprintText(clearance)));
                    }
                }
            }
            for (std::size_t i = 0; i < view.fixtures.size(); ++i) {
                const Fixture& first = *view.fixtures[i];
                if (isFloorCovering(first)) {
                    continue;
                }
                const Box firstBox = boxOf(first);
                for (std::size_t j = i + 1; j < view.fixtures.size(); ++j) {
                    const Fixture& second = *view.fixtures[j];
                    if (isFloorCovering(second)) {
                        continue;
                    }
                    const Box secondBox = boxOf(second);
                    if (firstBox.overlaps(secondBox)) {
                        m_findings.add(rule::kFootprintsOverlap, first.id.toString(),
                                       fmt::format("the fixture ({}, y {} to {}) overlaps {} ({}, y {} to {})",
                                                   floorText(firstBox), mmText(firstBox.minY), mmText(firstBox.maxY),
                                                   second.id.toString(), floorText(secondBox), mmText(secondBox.minY),
                                                   mmText(secondBox.maxY)));
                    }
                }
            }
        }
    }

    void checkLights() {
        for (const RoomLights& lights : m_context.lights) {
            const auto room = m_rooms.find(lights.room);
            const std::uint32_t limit = room != m_rooms.end() && room->second.roomTemplate != nullptr
                                            ? room->second.roomTemplate->lightLimit
                                            : kMaxSceneLights;
            if (lights.lights > limit) {
                m_findings.add(rule::kLightsOverLimit, lights.room.toString(),
                               fmt::format("the room holds {} point and spot lights; at most {} are allowed",
                                           lights.lights, limit));
            }
        }
    }

    void checkSlots() {
        std::map<SlotId, std::set<GameId>> games;
        for (const Placement& placement : m_plan.placements) {
            const std::string subject = placement.slot.toString();
            games[placement.slot].insert(placement.game);
            switch (lookUpSlot(m_fixtures, placement.slot)) {
            case SlotLookup::Exists:
                break;
            case SlotLookup::NoFixture:
                m_findings.add(rule::kSlotUnknown, subject,
                               fmt::format("{} stands in {}, which the plan does not hold", placement.game.toString(),
                                           placement.slot.fixture().toString()));
                break;
            case SlotLookup::NoLevel:
                m_findings.add(rule::kSlotUnknown, subject,
                               fmt::format("{} stands on level {}, which {} does not list", placement.game.toString(),
                                           placement.slot.level(), placement.slot.fixture().toString()));
                break;
            case SlotLookup::OutOfRange: {
                const FixtureLevel* level = levelOf(*m_fixtures.at(placement.slot.fixture()), placement.slot.level());
                m_findings.add(rule::kSlotUnknown, subject,
                               fmt::format("{} stands in slot {} of level {}, which has {} slots",
                                           placement.game.toString(), placement.slot.index(), placement.slot.level(),
                                           level->slots));
                break;
            }
            }
        }
        for (const auto& [slot, holders] : games) {
            if (holders.size() > 1) {
                std::string names;
                for (const GameId& game : holders) {
                    names += (names.empty() ? "" : ", ") + game.toString();
                }
                m_findings.add(rule::kSlotShared, slot.toString(),
                               fmt::format("the slot holds {} games: {}", holders.size(), names));
            }
        }
    }

    void checkDressing() {
        std::map<LevelId, std::vector<const DressedLevel*>> spans;
        for (const DressedLevel& dressed : m_plan.dressing) {
            const std::string subject = dressed.level.toString();
            const auto fixture = m_fixtures.find(dressed.level.fixture());
            if (fixture == m_fixtures.end()) {
                m_findings.add(rule::kDressingUnknown, subject,
                               fmt::format("the dressed level's fixture {} is not in the plan",
                                           dressed.level.fixture().toString()));
                continue;
            }
            const FixtureLevel* level = levelOf(*fixture->second, dressed.level.level());
            if (level == nullptr) {
                m_findings.add(rule::kDressingUnknown, subject,
                               fmt::format("{} does not list level {}", dressed.level.fixture().toString(),
                                           dressed.level.level()));
                continue;
            }
            if (dressed.count == 0 || static_cast<std::uint64_t>(dressed.firstSlot) + dressed.count > level->slots) {
                m_findings.add(rule::kDressingUnknown, subject,
                               fmt::format("the dressing covers {} slots from slot {}; the level has {} slots",
                                           dressed.count, dressed.firstSlot, level->slots));
                continue;
            }
            spans[dressed.level].push_back(&dressed);
        }
        for (auto& [level, list] : spans) {
            std::sort(list.begin(), list.end(),
                      [](const DressedLevel* a, const DressedLevel* b) { return a->firstSlot < b->firstSlot; });
            for (std::size_t i = 1; i < list.size(); ++i) {
                const DressedLevel& previous = *list[i - 1];
                if (previous.firstSlot + previous.count > list[i]->firstSlot) {
                    m_findings.add(rule::kDressingOverlap, level.toString(),
                                   fmt::format("the span of {} slots from slot {} overlaps the span from slot {}",
                                               previous.count, previous.firstSlot, list[i]->firstSlot));
                }
            }
        }
        for (const Placement& placement : m_plan.placements) {
            const auto found = spans.find(placement.slot.levelId());
            if (found == spans.end()) {
                continue;
            }
            for (const DressedLevel* dressed : found->second) {
                if (placement.slot.index() >= dressed->firstSlot &&
                    placement.slot.index() < dressed->firstSlot + dressed->count) {
                    m_findings.add(rule::kSlotDressed, placement.slot.toString(),
                                   fmt::format("{} stands in the dressed span of {} slots from slot {}",
                                               placement.game.toString(), dressed->count, dressed->firstSlot));
                }
            }
        }
        m_dressing = std::move(spans);
    }

    void checkGames() {
        for (const auto& [game, entries] : m_entries) {
            if (entries.placements + entries.unplaced > 1) {
                m_findings.add(rule::kGameTwice, game.toString(),
                               fmt::format("the game has {} placements and {} unplaced entries", entries.placements,
                                           entries.unplaced));
            }
        }
        for (const RoomSelection& selection : m_context.selections) {
            std::set<GameId> games;
            for (const RoomTitle& title : selection.titles) {
                games.insert(title.game);
            }
            for (const UnplacedGame& unplaced : selection.unplaced) {
                games.insert(unplaced.game);
            }
            for (const GameId& game : games) {
                if (!m_entries.contains(game)) {
                    m_findings.add(rule::kGameMissing, game.toString(),
                                   fmt::format("a game of the selection of {} has neither a placement nor an unplaced "
                                               "entry",
                                               selection.room.toString()));
                }
            }
        }
        for (const UnplacedGame& unplaced : m_plan.unplaced) {
            if (unplaced.reason != UnplacedReason::DuplicateTitle) {
                continue;
            }
            const std::string subject = unplaced.game.toString();
            if (unplaced.representative == unplaced.game || !unplaced.representative.isValid()) {
                m_findings.add(rule::kRepresentativeUnknown, subject,
                               "a duplicate title names itself, or no game, as its representative");
                continue;
            }
            const auto found = m_entries.find(unplaced.representative);
            if (found == m_entries.end() ||
                (found->second.placements == 0 && found->second.unplacedOtherThanDuplicate == 0)) {
                m_findings.add(rule::kRepresentativeUnknown, subject,
                               fmt::format("the representative {} is neither placed nor listed for another reason",
                                           unplaced.representative.toString()));
            }
        }
    }

    void checkPins() {
        for (const GamePin& pin : m_context.gamePins) {
            const std::string subject = pin.game.toString();
            const bool honoured =
                std::any_of(m_plan.placements.begin(), m_plan.placements.end(), [&](const Placement& placement) {
                    return placement.slot == pin.slot && placement.game == pin.game;
                });
            if (honoured) {
                continue;
            }
            if (lookUpSlot(m_fixtures, pin.slot) != SlotLookup::Exists) {
                m_findings.add(
                    rule::kPinOrphaned, subject,
                    fmt::format("the game is pinned to {}, which the plan does not hold", pin.slot.toString()));
            } else if (!m_entries.contains(pin.game)) {
                m_findings.add(rule::kPinOrphaned, subject,
                               fmt::format("the game pinned to {} is not in the plan", pin.slot.toString()));
            } else {
                m_findings.add(rule::kPinBroken, subject,
                               fmt::format("the game is pinned to {} but does not stand there", pin.slot.toString()));
            }
        }
        for (const FixturePin& pin : m_context.fixturePins) {
            const std::string subject = pin.fixture.toString();
            const auto room = m_rooms.find(pin.fixture.room());
            const bool socketExists = room != m_rooms.end() && room->second.roomTemplate != nullptr &&
                                      room->second.roomTemplate->socket(pin.fixture.socket()) != nullptr;
            if (!socketExists) {
                m_findings.add(rule::kPinOrphaned, subject,
                               "the fixture is pinned to a socket that its room's template no longer has");
                continue;
            }
            const auto fixture = m_fixtures.find(pin.fixture);
            if (fixture == m_fixtures.end()) {
                m_findings.add(rule::kPinBroken, subject, "the pinned fixture is not in the plan");
            } else if (pin.prefab && *pin.prefab != fixture->second->prefab) {
                m_findings.add(rule::kPinBroken, subject,
                               fmt::format("the fixture is pinned with the prefab {} but holds {}",
                                           pin.prefab->manifest, fixture->second->prefab.manifest));
            }
        }
    }

    /// A target's interaction point: the middle of its front face moved kInteractionDistanceMm out along its front.
    struct Target {
        const Fixture* fixture = nullptr;
        std::int64_t xMm = 0;
        std::int64_t zMm = 0;
    };

    static Target targetOf(const Fixture& fixture) {
        const Direction front = directionOf(fixture.yawCdeg);
        const auto [centreX, centreZ] = footprintCentreOf(fixture);
        // In half millimetres: half the depth is the depth, the distance twice itself.
        const double reach = static_cast<double>(fixture.depthMm) + 2.0 * static_cast<double>(kInteractionDistanceMm);
        return Target{&fixture, floorHalf(centreX + std::llround(front.sin * reach)),
                      floorHalf(centreZ + std::llround(front.cos * reach))};
    }

    void checkNavigation() {
        for (const auto& [id, view] : m_rooms) {
            if (view.roomTemplate == nullptr) {
                continue;
            }
            const Room& room = *view.room;
            const std::string subject = id.toString();
            std::vector<Target> targets;
            const Fixture* heroTv = nullptr;
            std::size_t heroTvs = 0;
            for (const Fixture* fixture : view.fixtures) {
                if (isHeroTv(*fixture)) {
                    heroTv = fixture;
                    ++heroTvs;
                }
                if (isCollectionFixture(fixture->role) || isHeroTv(*fixture)) {
                    targets.push_back(targetOf(*fixture));
                }
            }
            if (heroTvs != 1) {
                m_findings.add(rule::kHeroTvMissing, subject,
                               heroTvs == 0 ? std::string("the room has no hero TV (the hero corner's crt)")
                                            : fmt::format("the room has {} hero TVs", heroTvs));
            }

            const Footprint area{room.minXMm, room.minZMm, room.maxXMm, room.maxZMm};
            std::vector<Footprint> obstacles;
            for (const Fixture* fixture : view.fixtures) {
                if (!isFloorCovering(*fixture)) {
                    obstacles.push_back(floorOf(boxOf(*fixture)));
                }
            }
            const Footprint clearance = doorwayClearance(*view.roomTemplate, room.growthSteps);
            const std::int64_t startX = (clearance.minXMm + clearance.maxXMm) / 2;
            const std::int64_t startZ = (clearance.minZMm + clearance.maxZMm) / 2;

            const std::int64_t minClearance = (m_context.config.minClearWidthMm + 1) / 2;
            const NavGrid grid(area, obstacles, minClearance);
            const std::optional<NavCell> start = grid.cellAt(startX, startZ);
            const NavReach reach = start ? NavReach(grid, *start) : NavReach();
            const std::int64_t mainClearance = (m_context.config.mainPassageWidthMm + 1) / 2;
            const NavGrid mainGrid(area, obstacles, mainClearance);
            const std::optional<NavCell> mainStart = mainGrid.cellAt(startX, startZ);
            const NavReach mainReach = mainStart ? NavReach(mainGrid, *mainStart) : NavReach();

            m_targets += targets.size();
            if (!start || !grid.isFree(*start)) {
                m_findings.add(rule::kDoorwayBlocked, subject,
                               fmt::format("no body {} mm wide can stand at the doorway's clear area's centre (x {}, "
                                           "z {})",
                                           m_context.config.minClearWidthMm, startX, startZ));
            } else {
                for (const Target& target : targets) {
                    const bool reached = reach.reachesNear(target.xMm, target.zMm, kInteractionRadiusMm);
                    const bool reachedMain = mainReach.reachesNear(target.xMm, target.zMm, kInteractionRadiusMm);
                    if (reachedMain) {
                        ++m_targetsThroughMainPassage;
                    }
                    if (!reached) {
                        m_findings.add(rule::kUnreachable, target.fixture->id.toString(),
                                       fmt::format("no place {} mm wide within {} mm of its interaction point (x {}, z "
                                                   "{}) can be walked to from the doorway",
                                                   m_context.config.minClearWidthMm, kInteractionRadiusMm, target.xMm,
                                                   target.zMm));
                    } else if (!reachedMain) {
                        m_findings.add(rule::kPassageNarrow, target.fixture->id.toString(),
                                       fmt::format("its interaction point (x {}, z {}) is reached through a passage "
                                                   "narrower than {} mm",
                                                   target.xMm, target.zMm, m_context.config.mainPassageWidthMm));
                    }
                }
                if (!mainStart || !mainGrid.isFree(*mainStart)) {
                    m_findings.add(rule::kPassageNarrow, subject,
                                   fmt::format("the doorway's clear area leaves no passage {} mm wide",
                                               m_context.config.mainPassageWidthMm));
                }
            }

            ++m_heroRooms;
            if (heroTv != nullptr) {
                const Direction front = directionOf(heroTv->yawCdeg);
                const double depth = static_cast<double>(heroTv->depthMm);
                const auto [centreX, centreZ] = footprintCentreOf(*heroTv);
                const Box tvBox = boxOf(*heroTv);
                const std::array<double, 3> eye{2.0 * static_cast<double>(startX),
                                                2.0 * static_cast<double>(kDoorwayEyeHeightMm),
                                                2.0 * static_cast<double>(startZ)};
                const std::array<double, 3> screen{static_cast<double>(centreX) + front.sin * depth,
                                                   static_cast<double>(tvBox.minY + tvBox.maxY) / 2.0,
                                                   static_cast<double>(centreZ) + front.cos * depth};
                std::vector<std::string> blockers;
                for (const Fixture* fixture : view.fixtures) {
                    if (fixture != heroTv && !isFloorCovering(*fixture) &&
                        segmentCrosses(eye, screen, boxOf(*fixture))) {
                        blockers.push_back(fixture->id.toString());
                    }
                }
                if (blockers.empty()) {
                    ++m_heroVisible;
                } else {
                    std::string names;
                    for (const std::string& blocker : blockers) {
                        names += (names.empty() ? "" : ", ") + blocker;
                    }
                    m_findings.add(rule::kTvLineOfSight, heroTv->id.toString(),
                                   fmt::format("the hero TV is hidden from the doorway by {}", names));
                }
            }
        }
    }

    void checkSockets() {
        const std::int64_t density = std::clamp<std::int64_t>(m_context.config.decorationDensityPermille, 0, 1000);
        for (const auto& [id, view] : m_rooms) {
            if (view.roomTemplate == nullptr) {
                continue;
            }
            std::set<std::string> held;
            for (const Fixture* fixture : view.fixtures) {
                held.insert(fixture->id.socket());
            }
            std::size_t decorationSockets = 0;
            std::size_t decorated = 0;
            for (const TemplateSocket& socket : view.roomTemplate->sockets) {
                if (socket.growthStep > view.room->growthSteps) {
                    continue;
                }
                const bool holds = held.contains(socket.id);
                if ((socket.kind == SocketKind::HeroModule || socket.kind == SocketKind::SideWall) && !holds) {
                    m_findings.add(
                        rule::kSocketBare, socketIdText(*view.room, socket.id),
                        fmt::format("the {} socket holds neither a module nor decoration", toString(socket.kind)));
                }
                if (socket.kind == SocketKind::Decoration) {
                    ++decorationSockets;
                    decorated += holds ? 1 : 0;
                }
            }
            if (decorationSockets > 0) {
                const std::int64_t share =
                    static_cast<std::int64_t>(decorated) * 1000 / static_cast<std::int64_t>(decorationSockets);
                if (densityBand(share) != densityBand(density)) {
                    m_findings.add(rule::kDecorationDensity, id.toString(),
                                   fmt::format("{} of the {} decoration sockets hold decoration ({} permille), outside "
                                               "the band of the decoration density {} permille ({})",
                                               decorated, decorationSockets, share, density, densityBandText(density)));
                }
            }
        }
    }

    /// Style §6.3's bands of decoration density: essential, cozy curated, rich, showcase.
    static int densityBand(std::int64_t permille) noexcept {
        if (permille < 250) {
            return 0;
        }
        if (permille < 600) {
            return 1;
        }
        return permille < 850 ? 2 : 3;
    }
    static std::string_view densityBandText(std::int64_t permille) noexcept {
        constexpr std::array<std::string_view, 4> kBands{"0 to 249", "250 to 599", "600 to 849", "850 to 1000"};
        return kBands[static_cast<std::size_t>(densityBand(permille))];
    }

    [[nodiscard]] std::int64_t layoutScore() const {
        detail::LayoutScoreTerms terms;
        if (m_targets > 0) {
            terms.navigationClearance = static_cast<std::int64_t>(m_targetsThroughMainPassage * 1000 / m_targets);
        }
        if (m_heroRooms > 0) {
            terms.heroVisibility = static_cast<std::int64_t>(m_heroVisible * 1000 / m_heroRooms);
            terms.screenVisibility = terms.heroVisibility;
        }
        std::uint64_t capacityUnplaced = 0;
        for (const UnplacedGame& unplaced : m_plan.unplaced) {
            capacityUnplaced += unplaced.reason == UnplacedReason::Capacity ? 1 : 0;
        }
        const std::uint64_t placed = m_plan.placements.size();
        if (placed + capacityUnplaced > 0) {
            terms.collectionCapacity = static_cast<std::int64_t>(placed * 1000 / (placed + capacityUnplaced));
        }
        std::int64_t west = 0;
        std::int64_t east = 0;
        std::set<PrefabRef> decorationPrefabs;
        std::int64_t decorations = 0;
        for (const auto& [id, view] : m_rooms) {
            for (const Fixture* fixture : view.fixtures) {
                if (fixture->role == FixtureRole::SideModule) {
                    const TemplateSocket* socket = socketOf(view, *fixture);
                    west += socket != nullptr && socket->wall == Wall::West ? 1 : 0;
                    east += socket != nullptr && socket->wall == Wall::East ? 1 : 0;
                } else if (fixture->role == FixtureRole::Decoration) {
                    decorationPrefabs.insert(fixture->prefab);
                    ++decorations;
                }
            }
        }
        if (west + east > 0) {
            terms.visualBalance = 1000 - 1000 * std::abs(west - east) / (west + east);
        }
        if (decorations > 0) {
            terms.decorationVariation = static_cast<std::int64_t>(decorationPrefabs.size()) * 1000 / decorations;
        }
        terms.collisions = m_findings.count(rule::kFootprintsOverlap);
        terms.blockedDoors = m_findings.count(rule::kDoorReserved) + m_findings.count(rule::kDoorwayClearance) +
                             m_findings.count(rule::kDoorwayBlocked);
        // Two adjacent levels of one fixture, each dressed with one prop, the same.
        std::map<FixtureId, std::map<std::uint32_t, std::set<PrefabRef>>> props;
        for (const auto& [level, spans] : m_dressing) {
            for (const DressedLevel* dressed : spans) {
                props[level.fixture()][level.level()].insert(dressed->prop);
            }
        }
        for (const auto& [fixture, levels] : props) {
            for (auto it = levels.begin(); it != levels.end(); ++it) {
                const auto next = std::next(it);
                if (next != levels.end() && next->first == it->first + 1 && it->second.size() == 1 &&
                    it->second == next->second) {
                    ++terms.repeatedPatterns;
                }
            }
        }
        return detail::layoutScorePermille(terms);
    }

    struct Entries {
        std::uint64_t placements = 0;
        std::uint64_t unplaced = 0;
        std::uint64_t unplacedOtherThanDuplicate = 0;
    };

    const WorldPlan& m_plan;
    const ValidationContext& m_context;
    Findings m_findings;
    std::map<RoomId, RoomView> m_rooms;
    std::map<FixtureId, const Fixture*> m_fixtures;
    std::map<GameId, Entries> m_entries;
    std::map<LevelId, std::vector<const DressedLevel*>> m_dressing;
    std::uint64_t m_targets = 0;
    std::uint64_t m_targetsThroughMainPassage = 0;
    std::uint64_t m_heroRooms = 0;
    std::uint64_t m_heroVisible = 0;
};

/// "Slot_<level>_<index>" as its numbers.
std::optional<std::pair<std::uint32_t, std::uint32_t>> parseSlotSocket(std::string_view id) noexcept {
    constexpr std::string_view kPrefix = "Slot_";
    if (!id.starts_with(kPrefix)) {
        return std::nullopt;
    }
    id.remove_prefix(kPrefix.size());
    const std::size_t separator = id.find('_');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const auto level = detail::parseDecimal(id.substr(0, separator), kMaxLevel);
    const auto index = detail::parseDecimal(id.substr(separator + 1), kMaxSlotIndex);
    if (!level || !index) {
        return std::nullopt;
    }
    return std::pair{*level, *index};
}

} // namespace

std::span<const ValidationRule> validationRules() noexcept {
    return kRules;
}

const ValidationRule* findValidationRule(std::string_view name) noexcept {
    const std::size_t index = ruleIndex(name);
    return index < kRules.size() ? &kRules[index] : nullptr;
}

PrefabSlotSource manifestPrefabSlots(std::filesystem::path assetRoot) {
    return [root = std::move(assetRoot)](const PrefabRef& prefab) -> Expected<std::vector<PrefabSlotLevel>> {
        if (!assets::isValidPrefabId(prefab.manifest)) {
            return makeError(ErrorCode::InvalidArgument, "not a prefab id", prefab.manifest);
        }
        const std::filesystem::path path =
            root / "manifests" / "prefabs" / core::pathFromUtf8(prefab.manifest + ".json");
        auto manifest = assets::loadPrefabManifest(path);
        if (!manifest) {
            return propagate(std::move(manifest.error()));
        }
        if (!manifest->runtime.generator) {
            return makeError(ErrorCode::Unsupported, "a model prefab's slots are not read", prefab.manifest);
        }
        if (*manifest->runtime.generator != prefab.kind) {
            return makeError(
                ErrorCode::ValidationFailed,
                fmt::format("the manifest builds the kind '{}', not '{}'", *manifest->runtime.generator, prefab.kind),
                prefab.manifest);
        }
        auto built = geometry::buildPrefab(*manifest->runtime.generator, manifest->runtime.params);
        if (!built) {
            return propagate(std::move(built.error()));
        }
        std::map<std::uint32_t, std::set<std::uint32_t>> levels;
        for (const geometry::Socket& socket : built->sockets) {
            if (socket.kind != geometry::SocketKind::ShelfSlot) {
                continue;
            }
            const auto slot = parseSlotSocket(socket.id);
            if (!slot) {
                return makeError(ErrorCode::ValidationFailed, "a slot socket is not named Slot_<level>_<index>",
                                 socket.id);
            }
            levels[slot->first].insert(slot->second);
        }
        std::vector<PrefabSlotLevel> result;
        for (const auto& [level, indices] : levels) {
            // Indices from 0 without a gap, as the plan's slot counts assume.
            if (*indices.rbegin() + 1 != indices.size()) {
                return makeError(ErrorCode::ValidationFailed,
                                 fmt::format("level {} of the prefab's slots has a gap in its indices", level),
                                 prefab.manifest);
            }
            result.push_back({level, static_cast<std::uint32_t>(indices.size())});
        }
        return result;
    };
}

ValidationReport validatePlan(const WorldPlan& plan, const ValidationContext& context) {
    return Validator(plan, context).run();
}

} // namespace rv::world
