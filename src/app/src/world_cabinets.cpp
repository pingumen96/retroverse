// The arcade cabinets playing their clips in the building (I5 step 4, ADR-0016 §6): WorldSession's side. Each frame the
// session tells the screen service which cabinets of the bound room ask for their clips (app::chooseCabinets: the
// focused or inspected one, else attract's nearest within 3 m after its dwell, none while the editor runs) with their
// collection's pool of video materials; after the service's frame, the cabinets whose screens show their clips have
// their fronts hidden on the static path and drawn with their slots' pooled materials on the dynamic path (an inspected
// or pulled one's copy carries it: CollectionInstance::appendRenderItems), the others their covers again. A cabinet's
// front follows its item by pick id, so a collection laid out again in place (a live update, the editor's save) keeps
// it; a collection dropped (a floor committed again, a ride) lets every one go.

#include "world_session.hpp"

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

#include <rv/app/app_logic.hpp>
#include <rv/render/gl_device.hpp>

#include "collection_instance.hpp"

namespace rv::app {

std::pair<std::vector<CabinetScreen>, CabinetPool> WorldSession::cabinetScreens(const scene::Camera& camera,
                                                                                 double timeSeconds, std::size_t slots,
                                                                                 const VideoSettings& video) {
    m_cabinetRequests.clear();
    CollectionInstance* collection = activeCollection();
    const std::optional<world::RoomId> room = m_interaction->activeRoomId();
    const std::optional<std::string>& bound = m_screenRooms.bound();
    // Only the bound room's cabinets (its games are the service's), in a room whose items are screens.
    if (collection == nullptr || !collection->screenItems() || !room || m_state != "room" || !bound ||
        *bound != room->toString()) {
        m_cabinetAttract.reset();
        return {};
    }
    std::optional<std::size_t> roomIndex;
    for (std::size_t index = 0; index < m_rooms.size(); ++index) {
        if (m_rooms[index].collection.get() == collection) {
            roomIndex = index;
        }
    }
    if (!roomIndex) {
        m_cabinetAttract.reset();
        return {};
    }
    const WorldInteraction& interaction = m_interaction->interaction();
    const ScreenSettings settings = video.screen(ScreenKind::Cabinet, room->toString());
    const bool editing = m_edit != nullptr;
    // Attract: in free roam, the eye in the room, the cabinets' screens in view.
    std::optional<world::PickId> attract;
    if (!editing && interaction.state() == WorldState::FreeRoam && settings.enabled && eyeInScreenRoom()) {
        const scene::Frustum frustum = camera.frustum();
        std::vector<CabinetCandidate> candidates;
        candidates.reserve(collection->itemCount());
        for (std::size_t item = 0; item < collection->itemCount(); ++item) {
            const core::Aabb& box = collection->itemBox(item);
            // The screen faces out of its item's front (+Z of the item's space: the family's front).
            const glm::vec3 normal = glm::mat3(collection->itemTransform(item)) * glm::vec3(0.0f, 0.0f, 1.0f);
            const float length = glm::length(normal);
            candidates.push_back(CabinetCandidate{collection->pickOf(item), box.center(),
                                                  length > 0.0f ? normal / length : glm::vec3(0.0f, 0.0f, 1.0f),
                                                  frustum.intersects(box)});
        }
        attract = m_cabinetAttract.update(candidates, camera.position(), timeSeconds);
    } else {
        m_cabinetAttract.reset();
    }
    CabinetChoice choice;
    choice.state = interaction.state();
    choice.focused = interaction.focused();
    choice.editing = editing;
    choice.showOnFocus = settings.showOnFocus;
    choice.attract = settings.enabled;
    choice.attractPick = attract;
    std::vector<CabinetScreen> screens;
    for (const CabinetPlay& play : chooseCabinets(choice)) {
        const std::optional<std::size_t> item = collection->itemOfPick(play.pick);
        const std::optional<catalog::GameId> game = collection->gameOf(play.pick);
        if (!item || !game) {
            continue;
        }
        CabinetScreen screen;
        screen.id = cabinetScreenId(room->toString(), play.pick);
        screen.gameId = game->toString();
        screen.screenClass = play.screenClass;
        screen.bounds = collection->itemBox(*item);
        // Held in the hand whenever the interaction holds the item: Inspect and a launch from it until the return.
        screen.inHand = cabinetInHand(interaction.state());
        m_cabinetRequests.emplace(screen.id, std::make_pair(*roomIndex, play.pick));
        screens.push_back(std::move(screen));
    }
    CabinetPool pool;
    if (!screens.empty()) {
        auto materials =
            collection->videoMaterials(m_graphics, slots, m_context.device().caps().uniformBufferOffsetAlignment);
        if (!materials) {
            spdlog::error("World: the cabinets' video materials: {}", materials.error().toString());
            m_cabinetRequests.clear();
            return {};
        }
        pool.materials = std::move(*materials);
        pool.screenSize = collection->screenSize();
    }
    return {std::move(screens), std::move(pool)};
}

void WorldSession::showCabinets(std::span<const CabinetShown> shown) {
    std::vector<CabinetFront> fronts;
    for (const CabinetShown& cabinet : shown) {
        const auto request = m_cabinetRequests.find(cabinet.id);
        if (request == m_cabinetRequests.end() || request->second.first >= m_rooms.size()) {
            continue;
        }
        fronts.push_back(CabinetFront{request->second.first, request->second.second, cabinet.material});
    }
    // The fronts no longer showing a clip, or showing it with another material, go back to their own.
    for (const CabinetFront& front : m_cabinetFronts) {
        const bool kept = std::any_of(fronts.begin(), fronts.end(), [&](const CabinetFront& next) {
            return next.room == front.room && next.pick == front.pick;
        });
        if (kept || front.room >= m_rooms.size() || !m_rooms[front.room].collection) {
            continue;
        }
        CollectionInstance& collection = *m_rooms[front.room].collection;
        if (const std::optional<std::size_t> item = collection.itemOfPick(front.pick)) {
            if (Status restored = collection.setVideoFront(*item, std::nullopt); !restored) {
                spdlog::error("World: a cabinet's front: {}", restored.error().toString());
            }
        }
    }
    for (const CabinetFront& front : fronts) {
        if (!m_rooms[front.room].collection) {
            continue;
        }
        CollectionInstance& collection = *m_rooms[front.room].collection;
        if (const std::optional<std::size_t> item = collection.itemOfPick(front.pick)) {
            if (Status set = collection.setVideoFront(*item, front.material); !set) {
                spdlog::error("World: a cabinet's front: {}", set.error().toString());
            }
        }
    }
    m_cabinetFronts = std::move(fronts);
}

void WorldSession::appendCabinetFronts(std::vector<scene::RenderItem>& items) const {
    for (const CabinetFront& front : m_cabinetFronts) {
        if (front.room >= m_rooms.size() || !m_rooms[front.room].collection) {
            continue;
        }
        const CollectionInstance& collection = *m_rooms[front.room].collection;
        // A pulled or inspected cabinet's copy carries its clip (CollectionInstance::appendRenderItems).
        if (&collection == activeCollection() && m_interaction->isPulled(front.pick)) {
            continue;
        }
        const std::optional<std::size_t> item = collection.itemOfPick(front.pick);
        if (!item) {
            continue;
        }
        if (auto render = collection.frontRenderItem(*item, front.material)) {
            items.push_back(*render);
        }
    }
}

void WorldSession::dropCabinetFronts() {
    for (const CabinetFront& front : m_cabinetFronts) {
        if (front.room >= m_rooms.size() || !m_rooms[front.room].collection) {
            continue;
        }
        CollectionInstance& collection = *m_rooms[front.room].collection;
        if (const std::optional<std::size_t> item = collection.itemOfPick(front.pick)) {
            static_cast<void>(collection.setVideoFront(*item, std::nullopt));
        }
    }
    m_cabinetFronts.clear();
    m_cabinetRequests.clear();
    m_cabinetAttract.reset();
}

} // namespace rv::app
