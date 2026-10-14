#include <rv/core/aabb.hpp>

#include <algorithm>
#include <cmath>

namespace rv::core {

Aabb Aabb::fromMinMax(const glm::vec3& minCorner, const glm::vec3& maxCorner) noexcept {
    Aabb box;
    box.min = glm::min(minCorner, maxCorner);
    box.max = glm::max(minCorner, maxCorner);
    return box;
}

Aabb Aabb::fromCenterHalfExtents(const glm::vec3& center, const glm::vec3& halfExtents) noexcept {
    const glm::vec3 h = glm::abs(halfExtents);
    return fromMinMax(center - h, center + h);
}

Aabb Aabb::fromPoints(std::span<const glm::vec3> points) noexcept {
    Aabb box;
    for (const glm::vec3& p : points) {
        box.expand(p);
    }
    return box;
}

bool Aabb::isEmpty() const noexcept {
    return min.x > max.x || min.y > max.y || min.z > max.z;
}

glm::vec3 Aabb::center() const noexcept {
    return isEmpty() ? glm::vec3(0.0f) : (min + max) * 0.5f;
}

glm::vec3 Aabb::size() const noexcept {
    return isEmpty() ? glm::vec3(0.0f) : (max - min);
}

glm::vec3 Aabb::halfExtents() const noexcept {
    return size() * 0.5f;
}

void Aabb::expand(const glm::vec3& point) noexcept {
    min = glm::min(min, point);
    max = glm::max(max, point);
}

void Aabb::expand(const Aabb& other) noexcept {
    if (other.isEmpty()) {
        return;
    }
    min = glm::min(min, other.min);
    max = glm::max(max, other.max);
}

Aabb Aabb::merged(const Aabb& other) const noexcept {
    Aabb result = *this;
    result.expand(other);
    return result;
}

Aabb Aabb::inflated(float amount) const noexcept {
    if (isEmpty()) {
        return *this;
    }
    Aabb result;
    result.min = min - glm::vec3(amount);
    result.max = max + glm::vec3(amount);
    return result;
}

bool Aabb::contains(const glm::vec3& point) const noexcept {
    return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y && point.z >= min.z &&
           point.z <= max.z;
}

bool Aabb::contains(const Aabb& other) const noexcept {
    if (isEmpty() || other.isEmpty()) {
        return false;
    }
    return contains(other.min) && contains(other.max);
}

bool Aabb::intersects(const Aabb& other) const noexcept {
    if (isEmpty() || other.isEmpty()) {
        return false;
    }
    return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y && max.y >= other.min.y &&
           min.z <= other.max.z && max.z >= other.min.z;
}

Aabb Aabb::transformed(const glm::mat4& transform) const noexcept {
    if (isEmpty()) {
        return *this;
    }
    // Arvo, "Transforming Axis-Aligned Bounding Boxes", Graphics Gems (1990).
    const glm::vec3 translation(transform[3]);
    Aabb result;
    result.min = translation;
    result.max = translation;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            const float a = transform[column][row] * min[column];
            const float b = transform[column][row] * max[column];
            result.min[row] += std::min(a, b);
            result.max[row] += std::max(a, b);
        }
    }
    return result;
}

std::optional<float> Aabb::intersectRay(const Ray& ray, float tMax) const noexcept {
    if (isEmpty()) {
        return std::nullopt;
    }
    float tEnter = 0.0f;
    float tExit = tMax;
    for (int axis = 0; axis < 3; ++axis) {
        const float origin = ray.origin[axis];
        const float direction = ray.direction[axis];
        if (std::fabs(direction) < 1e-12f) {
            // Parallel to this slab: miss unless the origin lies within it.
            if (origin < min[axis] || origin > max[axis]) {
                return std::nullopt;
            }
            continue;
        }
        const float inverse = 1.0f / direction;
        float t0 = (min[axis] - origin) * inverse;
        float t1 = (max[axis] - origin) * inverse;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
        if (tEnter > tExit) {
            return std::nullopt;
        }
    }
    return tEnter;
}

} // namespace rv::core
