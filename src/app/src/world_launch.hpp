#pragma once

// Launching from the world and "Mostra nella stanza" (I3 step 15a; ADR-0012 §6, §8), between the room's interaction
// (WorldHover over app::WorldInteraction) and the product UI's launches (ProductUi: A1's LaunchFlow, the catalog's
// resolution on its worker, the play session, the launch hooks):
//  - Avvia on the inspected item (InteractionResult::launch) asks the product UI for the game's launch by GameId, as
//    the quick catalog's Avvia does (ProductUi::launchWorldGame); the interaction stays Launching while it is decided,
//    waits and runs, and the launch's end (the return, a failure, a refusal: ProductUi::takeWorldLaunchEnded) goes back
//    to it (WorldInteraction::launchEnded), which restores the walker's pose and Inspect of the same item on its next
//    update. An Avvia the product UI cannot take now ends at once (busyLaunchRefusal()), the card saying so.
//  - Around the launch, the product UI's hooks do the rest (retroverse_app.cpp): the hero CRT's decoder stopped and its
//    thread waited for before the hand-over, opened again at the clip's position after the return
//    (ScreenService::launchStarting, launchEnded), and the cover loader paused while it runs (WorldSession::coverRound
//    reads ProductUi::launchActive()).
//  - "Mostra nella stanza": the quick catalog's card asks where each game stands (ProductUi::setRoomPlacement, the
//    plan shown's world::PlanIndex: roomPlacementOf()); its button closes the catalog and the next frame eases the
//    walker to the item's Focus pose, the item pulled out (WorldInteraction::showInRoom).
//  - The run report's world.launch record (RoomLaunchLog).
// Main thread only.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rv/app/run_report.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/app/world_launch_ui.hpp>
#include <rv/scene/viewpoint.hpp>
#include <rv/world/where_is.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::app {

class ProductUi;
class WorldHover;

class WorldLaunch {
public:
    /// The plan of the room shown (its where-is index for the quick catalog's card), or none (no room).
    void setPlan(const world::WorldPlan* plan);

    /// Before the frame's interaction: the room's placement given to the quick catalog when the plan or the room that
    /// holds the eye changed (the card shown fills again), a "Mostra nella stanza" taken to its item, the launch's end
    /// given to the interaction. `walker`: the walker's pose as the frame starts.
    void beforeInteraction(ProductUi& product, WorldHover& hover, const scene::WalkerPose& walker, std::uint64_t frame);
    /// After it: Avvia (`result.launch`) asked of the product UI, the restore after a launch recorded. `walker`: the
    /// pose the frame leaves the walker at (the result's, else the frame's start).
    void afterInteraction(const InteractionResult& result, ProductUi& product, WorldHover& hover,
                          const scene::WalkerPose& walker, std::uint64_t frame);

    /// The card's room key of the game `gameId` (roomPlacementOf; "" without a room).
    [[nodiscard]] std::string placementOf(std::string_view gameId, const WorldHover& hover) const;

    [[nodiscard]] const RoomLaunchRecord& record() const noexcept { return m_log.record(); }

private:
    std::optional<world::PlanIndex> m_index;
    /// Changes of the plan shown, and the last one given to the product UI with the interaction's room it was given in.
    std::uint64_t m_revision = 0;
    std::uint64_t m_given = 0;
    std::optional<world::RoomId> m_givenRoom;
    /// A launch asked of the product UI and not ended yet.
    bool m_launching = false;
    RoomLaunchLog m_log;
};

} // namespace rv::app
