#pragma once

#include <limits>
#include <optional>
#include <span>

#include <glm/glm.hpp>

#include <rv/core/math.hpp>

namespace rv::core {

/// Axis-aligned bounding box in meters. A default-constructed box is empty (min > max) and acts as the
/// identity for expand()/merged().
struct Aabb {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    [[nodiscard]] static Aabb fromMinMax(const glm::vec3& minCorner, const glm::vec3& maxCorner) noexcept;
    [[nodiscard]] static Aabb fromCenterHalfExtents(const glm::vec3& center, const glm::vec3& halfExtents) noexcept;
    [[nodiscard]] static Aabb fromPoints(std::span<const glm::vec3> points) noexcept;

    /// True when min > max on any axis (no point has been added).
    [[nodiscard]] bool isEmpty() const noexcept;
    [[nodiscard]] glm::vec3 center() const noexcept;
    /// max - min, or zero for an empty box.
    [[nodiscard]] glm::vec3 size() const noexcept;
    [[nodiscard]] glm::vec3 halfExtents() const noexcept;

    void expand(const glm::vec3& point) noexcept;
    void expand(const Aabb& other) noexcept;
    [[nodiscard]] Aabb merged(const Aabb& other) const noexcept;
    /// Grows (or shrinks, for negative values) every face by `amount`.
    [[nodiscard]] Aabb inflated(float amount) const noexcept;

    /// Inclusive containment.
    [[nodiscard]] bool contains(const glm::vec3& point) const noexcept;
    [[nodiscard]] bool contains(const Aabb& other) const noexcept;
    /// Touching boxes intersect. Empty boxes never intersect anything.
    [[nodiscard]] bool intersects(const Aabb& other) const noexcept;

    /// Tight box around the transformed original box (Arvo's method); conservative for rotations.
    [[nodiscard]] Aabb transformed(const glm::mat4& transform) const noexcept;

    /// Slab test. Returns the entry parameter t in [0, tMax] (0 when the origin is inside), or nothing.
    [[nodiscard]] std::optional<float> intersectRay(const Ray& ray,
                                                    float tMax = std::numeric_limits<float>::max()) const noexcept;

    bool operator==(const Aabb&) const = default;
};

} // namespace rv::core
