#pragma once

// GPU-free (part of rv_app_cli, header only): the room's side of launching from the world and of "Mostra nella stanza"
// (I3 step 15a; ADR-0012 §6, §8), which rv_app's WorldLaunch drives:
//  - roomPlacementOf(): where a game stands in the room shown, as the quick catalog's card words it (its button for a
//    placed game, else the sentence of its unplaced reason: the keys of rv/ui/quick_catalog_room.hpp, the words in
//    assets/ui/quick_catalog.rml);
//  - busyLaunchRefusal(): the outcome of an Avvia the product UI could not take (another launch decided, waiting or
//    running), so that the interaction comes back to Inspect at once with the card saying so;
//  - RoomLaunchLog: the run report's world.launch record, kept as the launches ask, end and are restored.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <rv/app/command_line.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/math.hpp>
#include <rv/scene/viewpoint.hpp>
#include <rv/ui/quick_catalog_room.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/where_is.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::app {

/// The card's room key of `game` in the plan `index` indexes, with the eye in `eyeRoom` (the interaction's room; none
/// in a hall): ui::quick_catalog::kRoomPlaced when the plan places it and that room holds its item (`inLayout`),
/// kRoomOtherRoom when the plan places it in another room (or the eye stands in a hall), its unplaced reason
/// (world::toString(UnplacedReason)) when the plan leaves it out, kRoomNotInRoom when the plan does not hold it (a
/// system without a room) or places it in the eye's room where it shows no item.
[[nodiscard]] inline std::string_view roomPlacementOf(const world::PlanIndex& index, const catalog::GameId& game,
                                                      bool inLayout, const std::optional<world::RoomId>& eyeRoom) {
    const std::optional<world::GameLocation> location = index.find(game);
    if (!location) {
        return ui::quick_catalog::kRoomNotInRoom;
    }
    if (location->placed()) {
        if (inLayout) {
            return ui::quick_catalog::kRoomPlaced;
        }
        return eyeRoom && location->room == *eyeRoom ? ui::quick_catalog::kRoomNotInRoom
                                                     : ui::quick_catalog::kRoomOtherRoom;
    }
    if (location->reason) {
        return world::toString(*location->reason);
    }
    return ui::quick_catalog::kRoomNotInRoom;
}

/// The reason of busyLaunchRefusal().
inline constexpr std::string_view kReasonLaunchBusy = "busy";

/// Avvia in the room while the product UI decides, waits for or runs another launch: launch_outcome::kNotReady,
/// reason kReasonLaunchBusy.
[[nodiscard]] inline LaunchMessage busyLaunchRefusal() {
    return LaunchMessage{std::string(launch_outcome::kNotReady), std::string(kReasonLaunchBusy),
                         "another launch is being decided or runs"};
}

/// A walker's pose as the run report writes poses (degrees).
[[nodiscard]] inline SpawnPoint spawnOf(const scene::WalkerPose& pose) {
    SpawnPoint spawn;
    spawn.feet = pose.feet;
    spawn.yawDeg = core::radiansToDegrees(pose.yaw);
    spawn.pitchDeg = core::radiansToDegrees(pose.pitch);
    return spawn;
}

[[nodiscard]] inline bool walkerPoseEqual(const scene::WalkerPose& a, const scene::WalkerPose& b) noexcept {
    return a.feet == b.feet && a.yaw == b.yaw && a.pitch == b.pitch;
}

/// The world.launch record of a run (RoomLaunchRecord), as the room's launches and "Mostra nella stanza" go.
class RoomLaunchLog {
public:
    /// A room's plan shows: the record is written.
    void bind() noexcept { m_record.bound = true; }

    /// Avvia on `request` with the walker at `walker`: asked of the product UI, or not taken (`accepted` false, with
    /// busyLaunchRefusal()).
    void launchAsked(const WorldLaunchRequest& request, const scene::WalkerPose& walker, std::uint64_t frame,
                     bool accepted) {
        ++m_record.launches;
        m_before = walker;
        m_record.walkerBefore = spawnOf(walker);
        m_record.walkerAfter.reset();
        m_record.poseRestored = false;
        RoomLaunchEventRecord event = eventAt(frame, "launch");
        event.game = request.game.toString();
        event.pickId = pickText(request.item);
        event.walker = spawnOf(walker);
        add(std::move(event));
        if (!accepted) {
            const LaunchMessage refusal = busyLaunchRefusal();
            RoomLaunchEventRecord refused = eventAt(frame, "refused");
            refused.outcome = refusal.code;
            refused.reason = refusal.reason;
            add(std::move(refused));
        }
        m_awaitingRestore = true;
    }

    /// The launch ended with `outcome` (the interaction told: launchEnded()).
    void launchEnded(const LaunchMessage& outcome, std::uint64_t frame) {
        if (outcome.code == launch_outcome::kReturned) {
            ++m_record.returned;
        } else {
            ++m_record.notReturned;
        }
        RoomLaunchEventRecord event = eventAt(frame, "ended");
        event.outcome = outcome.code;
        event.reason = outcome.reason;
        add(std::move(event));
    }

    /// After the interaction's update of a frame: the first frame after a launch whose state is neither Launching nor
    /// Returning records the restore (`walker`: the pose the frame placed the walker at).
    void observe(WorldState state, world::PickId focused, const scene::WalkerPose& walker, std::uint64_t frame) {
        if (!m_awaitingRestore || state == WorldState::Launching || state == WorldState::Returning) {
            return;
        }
        m_awaitingRestore = false;
        m_record.walkerAfter = spawnOf(walker);
        m_record.poseRestored = m_before && walkerPoseEqual(*m_before, walker);
        RoomLaunchEventRecord event = eventAt(frame, "restored");
        event.state = std::string(toString(state));
        event.pickId = pickText(focused);
        event.walker = spawnOf(walker);
        add(std::move(event));
    }

    /// "Mostra nella stanza" on `game`: taken to the item `item` (an empty `failure`), or refused for `failure` (the
    /// card's room key, "item-unavailable", "state").
    void showInRoom(const catalog::GameId& game, world::PickId item, std::uint64_t frame, std::string_view failure) {
        RoomLaunchEventRecord event = eventAt(frame, failure.empty() ? "show-in-room" : "show-in-room-failed");
        event.game = game.toString();
        event.pickId = pickText(item);
        event.reason = std::string(failure);
        ++(failure.empty() ? m_record.showInRoom : m_record.showInRoomFailed);
        add(std::move(event));
    }

    /// The quick catalog's card asked where `gameId` stands and was told `key`.
    void placementAnswered(std::string_view gameId, std::string_view key) {
        ++m_record.placementQueries;
        m_record.placementGame = std::string(gameId);
        m_record.placement = std::string(key);
    }

    [[nodiscard]] const RoomLaunchRecord& record() const noexcept { return m_record; }

private:
    [[nodiscard]] static std::string pickText(world::PickId id) {
        return id == world::kNoPick ? std::string() : fmt::format("{:08X}", id);
    }
    [[nodiscard]] static RoomLaunchEventRecord eventAt(std::uint64_t frame, std::string_view kind) {
        RoomLaunchEventRecord event;
        event.frame = frame;
        event.event = std::string(kind);
        return event;
    }
    void add(RoomLaunchEventRecord event) {
        if (m_record.events.size() >= kMaxInteractionEvents) {
            ++m_record.eventsDropped;
            return;
        }
        m_record.events.push_back(std::move(event));
    }

    RoomLaunchRecord m_record;
    std::optional<scene::WalkerPose> m_before;
    bool m_awaitingRestore = false;
};

} // namespace rv::app
