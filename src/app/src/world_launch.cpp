#include "world_launch.hpp"

#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/app/launch_outcome.hpp>
#include <rv/ui/quick_catalog_room.hpp>

#include "product_ui.hpp"
#include "world_hover.hpp"

namespace rv::app {

void WorldLaunch::setPlan(const world::WorldPlan* plan) {
    if (plan != nullptr) {
        m_index.emplace(*plan);
        m_log.bind();
    } else {
        m_index.reset();
    }
    ++m_revision;
}

std::string WorldLaunch::placementOf(std::string_view gameId, const WorldHover& hover) const {
    if (!m_index) {
        return {};
    }
    const auto game = catalog::GameId::parse(gameId);
    if (!game) {
        return std::string(ui::quick_catalog::kRoomNotInRoom);
    }
    return std::string(roomPlacementOf(*m_index, *game, hover.pickOf(*game) != world::kNoPick, hover.activeRoomId()));
}

void WorldLaunch::beforeInteraction(ProductUi& product, WorldHover& hover, const scene::WalkerPose& walker,
                                    std::uint64_t frame) {
    // The quick catalog's card asks the room shown (its card fills again when the plan or the eye's room changed).
    if (std::optional<world::RoomId> room = hover.activeRoomId(); m_given != m_revision || room != m_givenRoom) {
        m_given = m_revision;
        m_givenRoom = std::move(room);
        if (m_index) {
            product.setRoomPlacement([this, &hover](std::string_view gameId) {
                std::string key = placementOf(gameId, hover);
                m_log.placementAnswered(gameId, key);
                return key;
            });
        } else {
            product.setRoomPlacement({});
        }
    }

    static_cast<void>(walker);
    // "Mostra nella stanza" is the session's since I4 step 7b (across rooms and floors: WorldSession::showInRoom,
    // world_ride.cpp), which records it here (showInRoom()).

    // The launch's end: the interaction restores the pose and Inspect of the item on this frame's update.
    if (m_launching) {
        if (const std::optional<LaunchMessage> ended = product.takeWorldLaunchEnded()) {
            m_launching = false;
            spdlog::info("Frame {}: the room's launch ended ({}{}{}): back to the item", frame, ended->code,
                         ended->reason.empty() ? "" : ", ", ended->reason);
            m_log.launchEnded(*ended, frame);
            hover.launchEnded(*ended);
        }
    }
}

world::PickId WorldLaunch::showInRoom(const catalog::GameId& game, WorldHover& hover, const scene::WalkerPose& walker,
                                      std::uint64_t frame) {
    const world::PickId id = hover.showInRoom(game, walker);
    std::string failure;
    if (id == world::kNoPick) {
        const std::string placement = placementOf(game.toString(), hover);
        failure = placement != ui::quick_catalog::kRoomPlaced ? placement
                  : hover.interaction().notice() == InteractionNotice::ItemUnavailable
                      ? std::string("item-unavailable")
                      : std::string("state");
        spdlog::warn("Frame {}: Mostra nella stanza on {}: not taken ({})", frame, game.toString(), failure);
    } else {
        spdlog::info("Frame {}: Mostra nella stanza on {}: the walker goes to item {:08X}", frame, game.toString(), id);
    }
    m_log.showInRoom(game, id, frame, failure);
    return id;
}

void WorldLaunch::showInRoomRefused(const catalog::GameId& game, std::string_view failure, std::uint64_t frame) {
    spdlog::warn("Frame {}: Mostra nella stanza on {}: not taken ({})", frame, game.toString(), failure);
    m_log.showInRoom(game, world::kNoPick, frame, failure);
}

void WorldLaunch::afterInteraction(const InteractionResult& result, ProductUi& product, WorldHover& hover,
                                   const scene::WalkerPose& walker, std::uint64_t frame) {
    if (result.launch) {
        const world::CollectionItem* item = hover.item(result.launch->item);
        const std::string title = item != nullptr ? item->title : std::string();
        // I5 step 9: a held launch (the insertion runs) is resolved meanwhile, its hand-over waiting for the insertion.
        const bool accepted =
            !m_launching && product.launchWorldGame(result.launch->game, title, result.launch->held);
        // The walker as Avvia found it: the pose the return restores (WorldInteraction::launchRecord()).
        const scene::WalkerPose before =
            hover.interaction().launchRecord() ? hover.interaction().launchRecord()->walker : walker;
        m_log.launchAsked(*result.launch, before, frame, accepted);
        if (accepted) {
            m_launching = true;
        } else {
            // Nothing will start: the interaction comes back to the item at once, the card saying why.
            const LaunchMessage refusal = busyLaunchRefusal();
            spdlog::warn("Frame {}: Avvia in the room on {}: the product UI takes no launch now", frame,
                         result.launch->game.toString());
            m_log.launchEnded(refusal, frame);
            hover.launchEnded(refusal);
        }
    }
    if (result.insertionEnded) {
        // The insertion ended (arrived or skipped): the launch's hand-over may begin, once its decoders stopped.
        spdlog::info("Frame {}: the insertion into the console {}: the launch goes on", frame,
                     *result.insertionEnded == InsertionEnd::Arrived ? "arrived" : "was skipped");
        m_log.insertionEnded(*result.insertionEnded, frame);
        if (m_launching) {
            product.releaseWorldLaunch();
        }
    }
    const WorldInteraction& interaction = hover.interaction();
    m_log.observe(interaction.state(), interaction.focused(), walker, frame);
}

} // namespace rv::app
