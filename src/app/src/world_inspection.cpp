#include "world_inspection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <fmt/format.h>

#include <rv/app/world_inspect.hpp>
#include <rv/core/math.hpp>

namespace rv::app {
namespace {

void widen(std::optional<double>& low, std::optional<double>& high, double value) {
    low = low ? std::min(*low, value) : value;
    high = high ? std::max(*high, value) : value;
}

/// The box of the item's cluster (its shelf level, its display case), or an empty one.
[[nodiscard]] core::Aabb clusterBounds(const world::CollectionLayout& layout, const world::CollectionItem& item) {
    const auto cluster =
        std::find_if(layout.clusters.begin(), layout.clusters.end(),
                     [&](const world::CollectionCluster& candidate) { return candidate.id == item.cluster; });
    return cluster != layout.clusters.end() ? cluster->bounds : item.worldBounds();
}

} // namespace

void WorldInspection::observe(const WorldInteraction& interaction, WorldState before, const InteractionResult& result) {
    if (interaction.state() == WorldState::Inspect && before != WorldState::Inspect &&
        before != WorldState::Returning) {
        ++m_record.inspections;
    }
    if (before == WorldState::Inspect || interaction.state() == WorldState::Inspect) {
        m_record.mediaRequests += result.showMedia ? 1 : 0;
        m_record.infoRequests += result.showInfo ? 1 : 0;
    }
    if (before == WorldState::Inspect && result.openCatalog) {
        ++m_record.catalogOpenedInInspect;
    }
}

void WorldInspection::observeInsertion(const WorldInteraction& interaction, const InteractionResult& result,
                                       float deltaSeconds, std::uint64_t frame) {
    m_frame = frame;
    const float dt = std::isfinite(deltaSeconds) && deltaSeconds > 0.0f ? deltaSeconds : 0.0f;
    if (result.launch && result.launch->held) {
        ++m_insertion.started;
        m_inserting = true;
        m_insertion.pickId = fmt::format("{:08X}", result.launch->item);
        m_insertion.startFrame = frame;
        m_insertion.seatedFrame.reset();
        m_insertion.endFrame.reset();
        m_insertion.end.clear();
        m_insertion.seconds = 0.0;
        m_insertion.frames.clear();
    }
    if (!m_inserting) {
        return;
    }
    m_insertion.seconds += dt;
    if (!m_insertion.seatedFrame && interaction.insertion() >= 1.0f) {
        m_insertion.seatedFrame = frame;
    }
    if (result.insertionEnded) {
        ++(*result.insertionEnded == InsertionEnd::Arrived ? m_insertion.arrived : m_insertion.skipped);
        m_insertion.end = std::string(toString(*result.insertionEnded));
        m_insertion.endFrame = frame;
        m_inserting = false;
    } else if (interaction.state() != WorldState::AnimatingToConsole) {
        // The launch ended during the insertion (refused, not ready): back to Inspect with the item in the hand.
        ++m_insertion.refused;
        m_insertion.end = "refused";
        m_insertion.endFrame = frame;
        m_inserting = false;
    }
}

world::PickId WorldInspection::markedItem(const WorldInteraction& interaction) noexcept {
    const world::PickId highlighted = interaction.highlighted();
    if (highlighted != world::kNoPick) {
        return highlighted;
    }
    return interaction.inspectView().lift > 0.0f ? interaction.focused() : world::kNoPick;
}

glm::mat4 WorldInspection::itemModel(const WorldInteraction& interaction, const CollectionRoom& room,
                                     const world::CollectionItem& item, const glm::mat4& pulled,
                                     const scene::Camera& camera, const std::optional<scene::RigidPose>& hand) {
    const InspectView view = interaction.inspectView();
    if (item.pickId != interaction.focused() || !(view.lift > 0.0f)) {
        return pulled;
    }
    // In the hand (VR) the item stands where the hand holds it: no fixture limits it.
    const float limit = hand ? std::numeric_limits<float>::infinity()
                             : heldDistanceLimit(clusterBounds(room.layout(), item), heldRadius(item),
                                                 camera.position(), camera.forward());
    const float drawn = heldDistance(view.distance, limit);
    const glm::mat4 model = heldItemTransform(pulled, view, camera, drawn, hand);

    // I5 step 9: from the hand into the console's media socket.
    const float insertion = interaction.insertion();
    if (insertion > 0.0f && room.consoleSocket()) {
        const ConsoleSocket& socket = *room.consoleSocket();
        const float scale = insertedScale(itemExtents(item), socket.opening);
        const glm::mat4 inserted = insertedItemTransform(model, socket, scale, insertion);
        const glm::vec3 center(inserted[3]);
        const float distance = glm::distance(center, socket.position());
        m_insertion.socket = socket.position();
        m_insertion.center = center;
        m_insertion.scale = insertedScaleAt(scale, insertion);
        m_insertion.distance = distance;
        ++(insertion >= 1.0f ? m_insertion.socketFrames : m_insertion.movingFrames);
        if (m_insertion.frames.size() < kMaxInsertionFrames &&
            (m_insertion.frames.empty() || m_insertion.frames.back().frame != m_frame)) {
            m_insertion.frames.push_back(InsertionFrameRecord{m_frame, insertion, distance});
        }
        return inserted;
    }

    // The frame's record: the pose in the camera's frame (x right, y up, z towards the viewer).
    const glm::mat4 toCamera = camera.view();
    m_record.pickId = fmt::format("{:08X}", item.pickId);
    m_record.lift = view.lift;
    m_record.yawDeg = core::radiansToDegrees(view.yaw);
    m_record.pitchDeg = core::radiansToDegrees(view.pitch);
    m_record.distance = view.distance;
    m_record.drawnDistance = drawn;
    m_record.heldCenter = glm::vec3(toCamera * model[3]);
    m_record.heldFront = glm::normalize(glm::vec3(toCamera * glm::vec4(glm::vec3(model[2]), 0.0f)));
    m_record.heldUp = glm::normalize(glm::vec3(toCamera * glm::vec4(glm::vec3(model[1]), 0.0f)));
    m_record.heroFront = m_heroFront;
    if (view.lift >= 1.0f) {
        ++m_record.heldFrames;
        widen(m_record.minDistance, m_record.maxDistance, view.distance);
        widen(m_record.minDrawnDistance, m_record.maxDrawnDistance, drawn);
    } else {
        ++m_record.liftFrames;
    }
    return model;
}

void WorldInspection::apply(const WorldInteraction& interaction, gl::RenderView& view) {
    const float lift = interaction.inspectView().lift;
    if (!(lift > 0.0f) || interaction.focused() == world::kNoPick) {
        return;
    }
    // I5 step 9: the dimming fades out as the item goes into the console.
    const InspectDimming dimming = insertionDimming(lift, interaction.insertion(), view.post.vignette);
    view.outline.dimOutside = dimming.dimOutside;
    view.outline.edge = interaction.highlighted() != world::kNoPick;
    view.post.vignette = dimming.vignette;
    ++m_record.dimmedFrames;
}

} // namespace rv::app
