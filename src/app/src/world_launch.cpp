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
    return std::string(roomPlacementOf(*m_index, *game, hover.pickOf(*game) != world::kNoPick));
}

void WorldLaunch::beforeInteraction(ProductUi& product, WorldHover& hover, const scene::WalkerPose& walker,
                                    std::uint64_t frame) {
    // The quick catalog's card asks the room shown (its card fills again when the room changed).
    if (m_given != m_revision) {
        m_given = m_revision;
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

    // "Mostra nella stanza", pressed in the catalog that closed at the end of the last frame.
    if (const std::optional<std::string> asked = product.takeShowInRoom()) {
        const auto game = catalog::GameId::parse(*asked);
        if (!game) {
            spdlog::warn("Frame {}: Mostra nella stanza on '{}': not a GameId", frame, *asked);
        } else {
            const world::PickId id = hover.showInRoom(*game, walker);
            std::string failure;
            if (id == world::kNoPick) {
                const std::string placement = placementOf(*asked, hover);
                failure = placement != ui::quick_catalog::kRoomPlaced ? placement
                          : hover.interaction().notice() == InteractionNotice::ItemUnavailable
                              ? std::string("item-unavailable")
                              : std::string("state");
                spdlog::warn("Frame {}: Mostra nella stanza on {}: not taken ({})", frame, game->toString(), failure);
            } else {
                spdlog::info("Frame {}: Mostra nella stanza on {}: the walker goes to item {:08X}", frame,
                             game->toString(), id);
            }
            m_log.showInRoom(*game, id, frame, failure);
        }
    }

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

void WorldLaunch::afterInteraction(const InteractionResult& result, ProductUi& product, WorldHover& hover,
                                   const scene::WalkerPose& walker, std::uint64_t frame) {
    if (result.launch) {
        const world::CollectionItem* item = hover.item(result.launch->item);
        const std::string title = item != nullptr ? item->title : std::string();
        const bool accepted = !m_launching && product.launchWorldGame(result.launch->game, title);
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
    const WorldInteraction& interaction = hover.interaction();
    m_log.observe(interaction.state(), interaction.focused(), walker, frame);
}

} // namespace rv::app
