#include <rv/scene/debug_lines.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

#include <rv/core/math.hpp>

namespace rv::scene {
namespace {

using core::Aabb;

/// Most lines a grid draws along each axis.
constexpr float kMaxGridLinesPerAxis = 1000.0f;
/// Socket axis lengths.
constexpr float kMinSocketAxis = 0.001f;
constexpr float kMaxSocketAxis = 0.5f;
constexpr float kDefaultSocketAxis = 0.1f;

/// Largest extent of a non-empty box, or nothing.
std::optional<float> largestExtent(const Aabb& box) noexcept {
    if (box.isEmpty()) {
        return std::nullopt;
    }
    const glm::vec3 size = box.size();
    const float largest = std::max({size.x, size.y, size.z});
    return std::isfinite(largest) ? std::optional<float>(largest) : std::nullopt;
}

} // namespace

void DebugLines::addLine(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color) {
    if (!core::isFinite(from) || !core::isFinite(to)) {
        return;
    }
    m_vertices.push_back(DebugVertex{from, color});
    m_vertices.push_back(DebugVertex{to, color});
}

void DebugLines::addBox(const core::Aabb& box, const glm::mat4& transform, const glm::vec4& color) {
    if (box.isEmpty()) {
        return;
    }
    // Corner i has max.x when bit 0 is set, max.y with bit 1, max.z with bit 2.
    std::array<glm::vec3, 8> corners{};
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 local((i & 1) != 0 ? box.max.x : box.min.x, (i & 2) != 0 ? box.max.y : box.min.y,
                              (i & 4) != 0 ? box.max.z : box.min.z);
        corners[static_cast<std::size_t>(i)] = glm::vec3(transform * glm::vec4(local, 1.0f));
    }
    // Every pair of corners that differs in exactly one bit is an edge.
    for (int i = 0; i < 8; ++i) {
        for (const int bit : {1, 2, 4}) {
            if ((i & bit) == 0) {
                addLine(corners[static_cast<std::size_t>(i)], corners[static_cast<std::size_t>(i | bit)], color);
            }
        }
    }
}

void DebugLines::addAxes(const glm::mat4& transform, float length) {
    const glm::vec3 origin(transform[3]);
    const std::array<std::pair<glm::vec3, glm::vec4>, 3> axes{{
        {glm::vec3(transform[0]), debug_colors::kAxisX},
        {glm::vec3(transform[1]), debug_colors::kAxisY},
        {glm::vec3(transform[2]), debug_colors::kAxisZ},
    }};
    for (const auto& [axis, color] : axes) {
        const float axisLength = glm::length(axis);
        if (std::isfinite(axisLength) && axisLength > 1e-6f) {
            addLine(origin, origin + axis * (length / axisLength), color);
        }
    }
}

void DebugLines::addGrid(float halfExtent, float spacing) {
    if (!std::isfinite(halfExtent) || !std::isfinite(spacing) || halfExtent <= 0.0f || spacing <= 0.0f) {
        return;
    }
    spacing = std::max(spacing, 2.0f * halfExtent / kMaxGridLinesPerAxis);
    const int steps = static_cast<int>(std::floor(halfExtent / spacing));
    for (int step = -steps; step <= steps; ++step) {
        const float offset = static_cast<float>(step) * spacing;
        // Lines along X (constant z) and along Z (constant x); the ones through the origin are the axes.
        addLine(glm::vec3(-halfExtent, 0.0f, offset), glm::vec3(halfExtent, 0.0f, offset),
                step == 0 ? debug_colors::kAxisX : debug_colors::kGrid);
        addLine(glm::vec3(offset, 0.0f, -halfExtent), glm::vec3(offset, 0.0f, halfExtent),
                step == 0 ? debug_colors::kAxisZ : debug_colors::kGrid);
    }
    addLine(glm::vec3(0.0f), glm::vec3(0.0f, 0.1f * halfExtent, 0.0f), debug_colors::kAxisY);
}

void DebugLines::append(const DebugLines& other) {
    m_vertices.insert(m_vertices.end(), other.m_vertices.begin(), other.m_vertices.end());
}

float socketAxisLength(const geometry::Socket& socket, const Aabb& objectBounds) noexcept {
    if (const std::optional<float> socketSize = largestExtent(socket.bounds)) {
        return std::clamp(0.5f * *socketSize, kMinSocketAxis, kMaxSocketAxis);
    }
    if (const std::optional<float> objectSize = largestExtent(objectBounds)) {
        return std::clamp(0.25f * *objectSize, kMinSocketAxis, kMaxSocketAxis);
    }
    return kDefaultSocketAxis;
}

void addSceneDebugLines(const SceneGeometry& geometry, DebugLines& lines) {
    for (const SceneObjectGeometry& object : geometry.objects) {
        for (const glm::mat4& instance : object.instances) {
            lines.addBox(object.bounds, instance, debug_colors::kObjectBounds);
            for (const geometry::Socket& socket : object.sockets) {
                lines.addAxes(instance * socket.transform, socketAxisLength(socket, object.bounds));
            }
        }
    }
}

} // namespace rv::scene
