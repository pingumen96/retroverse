#include "world_inspection.hpp"

#include <algorithm>
#include <cmath>

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

world::PickId WorldInspection::markedItem(const WorldInteraction& interaction) noexcept {
    const world::PickId highlighted = interaction.highlighted();
    if (highlighted != world::kNoPick) {
        return highlighted;
    }
    return interaction.inspectView().lift > 0.0f ? interaction.focused() : world::kNoPick;
}

glm::mat4 WorldInspection::itemModel(const WorldInteraction& interaction, const CollectionRoom& room,
                                     const world::CollectionItem& item, const glm::mat4& pulled,
                                     const scene::Camera& camera) {
    const InspectView view = interaction.inspectView();
    if (item.pickId != interaction.focused() || !(view.lift > 0.0f)) {
        return pulled;
    }
    const float limit =
        heldDistanceLimit(clusterBounds(room.layout(), item), heldRadius(item), camera.position(), camera.forward());
    const float drawn = heldDistance(view.distance, limit);
    const glm::mat4 model = heldItemTransform(pulled, view, camera, drawn);

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
    const InspectDimming dimming = inspectDimming(lift, view.post.vignette);
    view.outline.dimOutside = dimming.dimOutside;
    view.outline.edge = interaction.highlighted() != world::kNoPick;
    view.post.vignette = dimming.vignette;
    ++m_record.dimmedFrames;
}

} // namespace rv::app
