#pragma once

// The areas of a scene (scene_description.hpp's "areas"; ADR-0013 §9, I4 step 3): which area holds the viewer's eye and
// which of its lights a view packs. GPU-free: indices into the scene's lights, which the application packs into the
// renderer's lights block (gl::packLights over a selection).
//
// - The area that holds the eye is the first whose boxes contain it; while the eye is in none (in a wall, beyond the
//   floor), the last one that held it: a hysteresis, so that a scene with areas always has one. A tracker starts in the
//   area nearest its start point (the scene camera's eye).
// - A view packs, of that area's lights only: its directional lights, its key light (its first light with
//   castsShadows) and its other point and spot lights nearest the eye, up to the capacity of point and spot lights in
//   all (the nearest by squared distance from the eye, ties by the scene's order), the selection in the scene's order.
//   The other areas' lights are left out, so that their light neither leaks through walls nor counts against the
//   capacity; the ambient lights every area. An area of at most `capacity` point and spot lights packs them all,
//   whatever the eye's place in it, so its selection changes only when the eye changes areas.
// The work per frame is a pass over the eye's area's lights (a partial sort of their distances): microseconds.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include <rv/scene/light.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::scene {

/// The index of the first of `areas` whose boxes hold `point`, or nothing.
[[nodiscard]] std::optional<std::size_t> areaAt(std::span<const SceneArea> areas, const glm::vec3& point) noexcept;

/// The area holding `point` (areaAt()), else the one with a box nearest to it (ties: the first); nothing without areas.
[[nodiscard]] std::optional<std::size_t> nearestArea(std::span<const SceneArea> areas,
                                                     const glm::vec3& point) noexcept;

/// The lights of each area, as indices into `lights` in the scene's order, one list per area of `areas`; a light whose
/// area `areas` lacks is in none.
[[nodiscard]] std::vector<std::vector<std::uint32_t>> lightsByArea(std::span<const Light> lights,
                                                                   std::span<const SceneArea> areas);

/// Which area holds the eye, and the lights a view packs there (see the file comment). Empty (no areas): it selects
/// nothing and every call changes nothing.
class AreaLighting {
public:
    AreaLighting() = default;
    /// Starts in the area nearest `start` with the lights a view from there packs. `capacity` is the renderer's number
    /// of point and spot lights (gl::kMaxPunctualLights).
    AreaLighting(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& start,
                 std::size_t capacity);

    /// True for a scene without areas.
    [[nodiscard]] bool empty() const noexcept { return m_areaLights.empty(); }
    /// The index of the area that holds the eye; nothing without areas.
    [[nodiscard]] std::optional<std::size_t> area() const noexcept { return m_area; }
    /// The lights a view packs, as indices into the scene's lights in its order.
    [[nodiscard]] std::span<const std::uint32_t> selection() const noexcept { return m_selection; }

    /// Follows the eye to `eye` with the scene's lights as they are now (`lights`, the list this was made with, perhaps
    /// edited in place): the area that holds it and the lights a view packs there. True when the area or the
    /// selection changed.
    bool update(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& eye);
    /// The selection for the current area again, with `lights` as they are now (after an edit of their positions or
    /// flags); true when it changed.
    bool reselect(std::span<const Light> lights, const glm::vec3& eye);

private:
    void select(std::span<const Light> lights, const glm::vec3& eye, std::vector<std::uint32_t>& out);

    std::vector<std::vector<std::uint32_t>> m_areaLights;
    std::size_t m_capacity = 0;
    std::optional<std::size_t> m_area;
    std::vector<std::uint32_t> m_selection;
    /// Scratch of select(), kept to spare an allocation per frame.
    std::vector<std::uint32_t> m_next;
    std::vector<std::pair<float, std::uint32_t>> m_distances;
};

} // namespace rv::scene
