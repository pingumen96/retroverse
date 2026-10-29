#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or RmlUi header is included.
//
// The directory in the app (I4 step 7b; ADR-0013 §10; docs/i4/step-7a.md's contract), the parts that need no window:
//  - directoryBuildingOf(): the plan's floors in the elevator's order (building.json's, Floor::order), each floor's rooms
//    along its hall from the elevator (by their footprint's middle on the floor, the north row first on a tie), each
//    room's kind, its sections' systems with the catalog's full names and the games it places;
//    (I4b step 8, ADR-0015 §7: a chain's rooms together, the head first and its expansions by number, each with its
//    titles' ranges, chainRangesOf());
//  - floorRecordsOf(): the same floors for the run report (world.floors);
//  - directoryGameOf(): a favourite or recently played game (catalog::RecentGame) as the games tab shows it, its last
//    play the machine's local date;
//  - routeDirectoryCommand(), routeShowInRoom(): where a command of the directory, or "Mostra nella stanza" of the quick
//    catalog's card, takes the walker: a ride to another floor, a teleport on the floor shown, the Focus pose in the
//    room the interaction is in, or a refusal with the panel's notice (ui::directory::kNotices). The application then
//    does it (app::WorldSession).

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/elevator_ride.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/recent_games.hpp>
#include <rv/ui/directory_model.hpp>
#include <rv/world/where_is.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::app {

/// The directory's notices (ui::directory::kNotices): a ride or teleport refused for now, the building not planned yet.
inline constexpr std::string_view kNoticeRideUnavailable = "ride-unavailable";
inline constexpr std::string_view kNoticeBuildingPending = "building-pending";

/// A system's full name as the catalog has it ("" none: the directory shows its name).
using SystemFullName = std::function<std::string(const catalog::SystemName&)>;

/// I4b step 8 (ADR-0015 §7): a placed game as a chain's ranges read it: the system whose section it stands in and its
/// sort key (catalog::sortKeyOf() of its sort name or title, as the rooms' items files hold it).
struct ChainTitle {
    catalog::SystemName system;
    std::string sortKey;

    bool operator==(const ChainTitle&) const = default;
};

/// The title of a placed game; nothing when it is not known (an items file not written yet).
using ChainTitleOf = std::function<std::optional<ChainTitle>(const catalog::GameId&)>;

/// What a room of a chain shows of its titles (ui::DirectoryRoom::ranges and scattered, the systems by name only).
struct ChainRoomRanges {
    std::vector<ui::DirectoryRange> ranges;
    bool scattered = false;

    bool operator==(const ChainRoomRanges&) const = default;
};

/// The ranges of every room of a chain of more than one room in `plan` (I4b step 8; ADR-0015 §7 as amended on
/// 2026-10-29): the chain's placed titles whose title is known in the chain's alphabetical order (the sections in the
/// head's order, then the sort key, then the GameId: the order a fresh split shares them in, chainShares()), and for
/// each room, section by section in that order, the initials (rangeInitial()) of the first and the last of its titles
/// there: never a title of the room outside its range. Where one room's range ends on the initial the next one's (in
/// the order of their first titles) begins on, both ends show two letters (rangePrefix(): "A – Mo", "Mo – Y"). A room's titles interleave with another room's (`scattered`)
/// when another room of the chain holds a title between its first and its last in that order: an update's additions
/// (a new title goes after its alphabetical predecessor wherever that stands, else to the chain's next free slot,
/// wrapping to the head), never a fresh split. A room alone has no entry; a room of a chain none of whose titles is
/// known has an empty entry.
[[nodiscard]] std::map<world::RoomId, ChainRoomRanges> chainRangesOf(const world::WorldPlan& plan,
                                                                     const ChainTitleOf& titleOf);

/// The initial a range shows of a sort key: its first character, an ASCII letter upper-cased ("a" gives "A"), a digit
/// as itself, anything else (a sign, a letter beyond ASCII, nothing) "#".
[[nodiscard]] std::string rangeInitial(std::string_view sortKey);
/// The initial and, when it is a letter or a digit of ASCII, the sort key's second character in lower case ("mossy"
/// gives "Mo", "m-1" gives "M"): a boundary two rooms share the initial of.
[[nodiscard]] std::string rangePrefix(std::string_view sortKey);

/// The systems of `plan`'s rooms of a chain of more than one room (their sections), each once in order: the items
/// files chainTitlesOf() reads.
[[nodiscard]] std::vector<catalog::SystemName> chainSystemsOf(const world::WorldPlan& plan);

/// The titles of the placed games of `systems` from their rooms' items files under `cacheDir` (room_items.hpp; the
/// screen data's sort key, else catalog::sortKeyOf() of the title): read on the worker pool when the plan shown holds
/// a chain (WorldSession). A file missing or unreadable gives none of its games (logged).
[[nodiscard]] std::map<catalog::GameId, ChainTitle> chainTitlesOf(const std::filesystem::path& cacheDir,
                                                                  const catalog::LibraryId& library,
                                                                  const std::vector<catalog::SystemName>& systems);

/// The "Piani" tab's floors and rooms of `plan` (see the file comment), the walker nowhere ("" floor and room: the
/// caller sets DirectoryBuilding::currentFloor and currentRoom). `ranges` (I4b step 8): chainRangesOf()'s, the rooms'
/// ranges once their titles are known (none: no range yet).
[[nodiscard]] ui::DirectoryBuilding directoryBuildingOf(const world::WorldPlan& plan, const SystemFullName& fullName,
                                                        const std::map<world::RoomId, ChainRoomRanges>* ranges =
                                                            nullptr);

/// The plan's floors for the run report, in the elevator's order: their rooms and the games those place.
[[nodiscard]] std::vector<WorldFloorRecord> floorRecordsOf(const world::WorldPlan& plan);

/// The local calendar date of a UTC time.
using LocalDate = std::function<std::optional<ui::DirectoryDate>(catalog::UtcTime)>;
/// The machine's local date of `time` (std::chrono::current_zone()); its UTC date when the time zone database cannot be
/// read.
[[nodiscard]] std::optional<ui::DirectoryDate> machineLocalDate(catalog::UtcTime time);

/// `game` as the games tab shows it (ui::DirectoryGame): its GameId's text, title, system and full name (the system's
/// name when the catalog has none), whether it is a favourite, `played` the local date of its last play (`localDate`).
[[nodiscard]] ui::DirectoryGame directoryGameOf(const catalog::RecentGame& game, const LocalDate& localDate);

/// "YYYY-MM-DD" of a date ("" none): the run report's DirectoryGameRecord::played.
[[nodiscard]] std::string dateText(const std::optional<ui::DirectoryDate>& date);

/// What the world can do now, for a command of the directory or a "Mostra nella stanza".
struct WorldMoveConditions {
    /// A floor of a plan shows (not the shell, the test room or a building still being made).
    bool planReady = false;
    /// The floor shown is being applied: the persisted start's preview awaits its reconciliation, or a building's result
    /// is built again for the floor shown (ADR-0013 §10: no ride while a floor applies; no teleport either).
    bool applying = false;
    /// A ride runs.
    bool riding = false;
    /// A launch is decided, waits or runs (ProductUi::launchActive).
    bool launchActive = false;
    /// The world's interaction state (no ride or teleport in Focus, Inspect, Launching, Returning; "Mostra nella
    /// stanza" leaves Focus first).
    WorldState state = WorldState::FreeRoam;
};

/// Where a command takes the walker.
enum class WorldMoveKind : std::uint8_t {
    Ignore,   ///< nothing to do (a command the product UI handles itself: Launch, Close; a target the plan lacks)
    Refuse,   ///< not now: the directory reopens with WorldMove::notice
    Ride,     ///< ride to `floor` (then the room's doorway, the game's Focus pose)
    Teleport, ///< on the floor shown: to `room`'s doorway, or the elevator ("" room); then the game's Focus pose
    ShowHere, ///< the game's Focus pose in the room the interaction is in (I3's "Mostra nella stanza")
};

/// "ignore", "refuse", "ride", "teleport", "show-here".
[[nodiscard]] std::string_view toString(WorldMoveKind kind) noexcept;

struct WorldMove {
    WorldMoveKind kind = WorldMoveKind::Ignore;
    RideReason reason = RideReason::Floor;
    /// The destination floor (Ride; Teleport's own floor), the room ("" the elevator), the game (ShowInRoom).
    std::string floor;
    std::string room;
    std::string game;
    /// Refuse: the directory's notice (kNoticeRideUnavailable, kNoticeBuildingPending).
    std::string notice;
    /// Why, in English (the log).
    std::string why;

    bool operator==(const WorldMove&) const = default;
};

/// The world's side of `command` (ui::DirectoryCommand): ChooseFloor rides to that floor's elevator, or teleports to
/// the elevator of the floor shown (`currentFloor`); ChooseRoom rides to the room's floor, or teleports to its doorway
/// on the floor shown; ShowInRoom is routeShowInRoom(). Refused with kNoticeBuildingPending without a plan
/// (`plan`/`index` null or `conditions.planReady` false), with kNoticeRideUnavailable while a floor applies, a ride
/// runs, a launch is active or the interaction is not in FreeRoam. Launch, Close and targets the plan lacks: Ignore.
[[nodiscard]] WorldMove routeDirectoryCommand(const ui::DirectoryCommand& command, const WorldMoveConditions& conditions,
                                              const world::WorldPlan* plan, const world::PlanIndex* index,
                                              std::string_view currentFloor, std::string_view eyeRoom);

/// "Mostra nella stanza" of `game` (the quick catalog's card or the directory's): the plan places it in a room of the
/// floor shown, the room the interaction is in (`eyeRoom`): ShowHere; another room there: Teleport to its doorway, then
/// the Focus pose; a room of another floor: Ride there, then the same. Focus does not refuse it (it is left first);
/// Inspect, Launching and Returning, an active launch or a ride refuse it (kNoticeRideUnavailable), and so does a floor
/// applying, but for ShowHere (I3's "Mostra nella stanza" in the room the walker is in, which the persisted start's
/// preview allowed); no plan: kNoticeBuildingPending. A game the plan does not place (or whose room it lacks): Ignore.
[[nodiscard]] WorldMove routeShowInRoom(const catalog::GameId& game, const WorldMoveConditions& conditions,
                                        const world::PlanIndex* index, std::string_view currentFloor,
                                        std::string_view eyeRoom);

} // namespace rv::app
