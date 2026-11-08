#include <rv/xr/panels.hpp>

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

namespace rv::xr {
namespace {

constexpr glm::vec3 kUp(0.0f, 1.0f, 0.0f);

float finiteAspect(float aspect) noexcept {
    return std::isfinite(aspect) && aspect > 0.01f ? aspect : 16.0f / 9.0f;
}

} // namespace

glm::mat4 PanelPose::model() const noexcept {
    const glm::mat3 axes = glm::mat3_cast(glm::normalize(orientation));
    // The unit square's corner (0, 0) at the panel's bottom-left, its sides along the panel's axes.
    glm::mat4 matrix(1.0f);
    matrix[0] = glm::vec4(axes[0] * size.x, 0.0f);
    matrix[1] = glm::vec4(axes[1] * size.y, 0.0f);
    matrix[2] = glm::vec4(axes[2], 0.0f);
    matrix[3] = glm::vec4(center - 0.5f * size.x * axes[0] - 0.5f * size.y * axes[1], 1.0f);
    return matrix;
}

PanelPose worldPanel(const Pose& head, float aspect) noexcept {
    const float yaw = anglesOf(head.orientation).yaw;
    const glm::vec3 forward(std::sin(yaw), 0.0f, -std::cos(yaw));
    PanelPose panel;
    panel.center = head.position + forward * kWorldPanelDistance - kUp * kWorldPanelDrop;
    panel.orientation = glm::angleAxis(-yaw, kUp);
    panel.size = glm::vec2(kWorldPanelWidth, kWorldPanelWidth / finiteAspect(aspect));
    return panel;
}

PanelPose gameScreenPanel(const Pose& head, float aspect) noexcept {
    const float yaw = anglesOf(head.orientation).yaw;
    const glm::vec3 forward(std::sin(yaw), 0.0f, -std::cos(yaw));
    PanelPose panel;
    panel.center = head.position + forward * kGameScreenDistance;
    panel.orientation = glm::angleAxis(-yaw, kUp);
    panel.size = glm::vec2(kGameScreenWidth, kGameScreenWidth / finiteAspect(aspect));
    return panel;
}

scene::EyeFov hudField(float aspect) noexcept {
    const float across = std::tan(core::degreesToRadians(0.5f * kHudFieldDeg));
    const float up = across / finiteAspect(aspect);
    return scene::EyeFov{-std::atan(across), std::atan(across), std::atan(up), -std::atan(up)};
}

PanelPose hudPanel(const Pose& head, float aspect) noexcept {
    const glm::quat orientation = glm::normalize(head.orientation);
    const float across = std::tan(core::degreesToRadians(0.5f * kHudFieldDeg));
    PanelPose panel;
    panel.center = head.position + orientation * glm::vec3(0.0f, 0.0f, -kHudDistance);
    panel.orientation = orientation;
    panel.size = glm::vec2(2.0f * kHudDistance * across, 2.0f * kHudDistance * across / finiteAspect(aspect));
    return panel;
}

std::optional<PanelHit> panelHit(const PanelPose& panel, const core::Ray& ray, glm::vec2 canvasSize) noexcept {
    if (!core::isFinite(ray.origin) || !core::isFinite(ray.direction)) {
        return std::nullopt;
    }
    const glm::mat3 axes = glm::mat3_cast(glm::normalize(panel.orientation));
    const glm::vec3 normal = axes[2];
    // From the front only: the ray goes into the panel's face.
    const float facing = glm::dot(ray.direction, normal);
    if (!(facing < -1e-6f)) {
        return std::nullopt;
    }
    const float distance = glm::dot(panel.center - ray.origin, normal) / facing;
    if (!(distance > 0.0f)) {
        return std::nullopt;
    }
    const glm::vec3 local = ray.origin + ray.direction * distance - panel.center;
    const float u = glm::dot(local, axes[0]) / panel.size.x + 0.5f;
    const float v = glm::dot(local, axes[1]) / panel.size.y + 0.5f;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return std::nullopt;
    }
    return PanelHit{glm::vec2(u * canvasSize.x, (1.0f - v) * canvasSize.y), distance};
}

} // namespace rv::xr
