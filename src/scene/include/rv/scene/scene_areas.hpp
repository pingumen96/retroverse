#pragma once

// The areas of a scene (scene_description.hpp's "areas"; ADR-0013 §9, I4 step 3; ADR-0014 §4, I6 step 1): which area
// holds the viewer's eye, which lights a view packs there and the area's environment the views ease to. GPU-free:
// indices into the scene's lights, which the application packs into the renderer's lights block (gl::packLights over a
// selection).
//
// - The area that holds the eye is the first whose boxes contain it; while the eye is in none (in a wall, beyond the
//   floor), the last one that held it: a hysteresis, so that a scene with areas always has one. A tracker starts in the
//   area nearest its start point (the scene camera's eye).
// - Neighbours: two areas are neighbours where a box of one touches or overlaps a box of the other over a face (not
//   along an edge or at a corner), as a room's doorway passage touches its hall's interior; the boxes' common part is
//   their portal. Areas apart, as rooms behind a wall are, are no neighbours, so their lights never shine through it.
// - A view packs, first, the eye's area's own lights: its directional lights, its key light (its first light with
//   castsShadows) and its other point and spot lights nearest the eye (by squared distance, ties by the scene's
//   order), up to the capacity of point and spot lights. Given the view's direction, a point or spot light whose whole
//   range lies behind the eye (its distance behind the plane through the eye across the view at least its range) is
//   left out: it lights nothing the view can show, so leaving it out changes no pixel and frees its place (the key
//   light stays, for its shadow). Then, while there is room, up to kNeighbourLightsPerArea point
//   and spot lights of each neighbour: those nearest a portal they share, within kNeighbourReachM of it and within
//   their own range of it (a light that cannot reach the doorway cannot shine through it), the nearest the eye first
//   when they do not all fit. Each part of the selection is in the scene's order, the own lights before the
//   neighbours'; ownCount() tells them apart: a neighbour's light is never the key light and casts no shadow
//   (gl::packLights' keyCandidates). Other lights are left out, so that their light neither leaks through walls nor
//   counts against the capacity; the ambient lights every area.
// The work per frame is a pass over the eye's area's lights and its neighbours' (partial sorts of distances):
// microseconds.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include <rv/core/aabb.hpp>
#include <rv/scene/light.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::scene {

/// The most point and spot lights a view packs of each neighbouring area, and how far from the portal they may be.
inline constexpr std::size_t kNeighbourLightsPerArea = 4;
inline constexpr float kNeighbourReachM = 6.0f;
/// How long the views take to ease from one area's environment to the next's (EnvironmentEase).
inline constexpr float kAreaEnvironmentEaseSeconds = 0.5f;

/// The index of the first of `areas` whose boxes hold `point`, or nothing.
[[nodiscard]] std::optional<std::size_t> areaAt(std::span<const SceneArea> areas, const glm::vec3& point) noexcept;

/// The area holding `point` (areaAt()), else the one with a box nearest to it (ties: the first); nothing without areas.
[[nodiscard]] std::optional<std::size_t> nearestArea(std::span<const SceneArea> areas,
                                                     const glm::vec3& point) noexcept;

/// The lights of each area, as indices into `lights` in the scene's order, one list per area of `areas`; a light whose
/// area `areas` lacks is in none.
[[nodiscard]] std::vector<std::vector<std::uint32_t>> lightsByArea(std::span<const Light> lights,
                                                                   std::span<const SceneArea> areas);

/// An area's neighbour and the portals they share (the common parts of their touching boxes).
struct AreaNeighbour {
    std::size_t area = 0;
    std::vector<core::Aabb> portals;

    bool operator==(const AreaNeighbour&) const = default;
};

/// The neighbours of each area of `areas` (see the file comment), one list per area, by index.
[[nodiscard]] std::vector<std::vector<AreaNeighbour>> areaNeighbours(std::span<const SceneArea> areas);

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
    /// The lights a view packs, as indices into the scene's lights: the eye's area's own, then its neighbours'.
    [[nodiscard]] std::span<const std::uint32_t> selection() const noexcept { return m_selection; }
    /// How many of selection()'s first lights are the eye's area's own (the others are its neighbours').
    [[nodiscard]] std::size_t ownCount() const noexcept { return m_ownCount; }
    /// The neighbours of each area (areaNeighbours()).
    [[nodiscard]] const std::vector<std::vector<AreaNeighbour>>& neighbours() const noexcept { return m_neighbours; }

    /// Follows the eye to `eye` with the scene's lights as they are now (`lights`, the list this was made with, perhaps
    /// edited in place): the area that holds it and the lights a view packs there, `forward` the view's unit
    /// direction (zero: none, every light counts as in front). True when the area or the selection changed.
    bool update(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& eye,
                const glm::vec3& forward = glm::vec3(0.0f));
    /// The selection for the current area again, with `lights` as they are now (after an edit of their positions or
    /// flags); true when it changed.
    bool reselect(std::span<const Light> lights, const glm::vec3& eye, const glm::vec3& forward = glm::vec3(0.0f));

private:
    void select(std::span<const Light> lights, const glm::vec3& eye, const glm::vec3& forward,
                std::vector<std::uint32_t>& out, std::size_t& ownCount);

    std::vector<std::vector<std::uint32_t>> m_areaLights;
    std::vector<std::vector<AreaNeighbour>> m_neighbours;
    std::size_t m_capacity = 0;
    std::optional<std::size_t> m_area;
    std::vector<std::uint32_t> m_selection;
    std::size_t m_ownCount = 0;
    /// Scratch of select(), kept to spare an allocation per frame.
    std::vector<std::uint32_t> m_next;
    std::vector<std::pair<float, std::uint32_t>> m_distances;
    std::vector<std::pair<float, std::uint32_t>> m_neighbourDistances;
    std::vector<std::pair<float, std::uint32_t>> m_neighbourPicks;
};

/// True when `a` and `b` have the same area terms (exposure, ambient sky, ground and intensity, fill colour and
/// intensity), the terms an area may set (AreaEnvironment).
[[nodiscard]] bool sameAreaTerms(const EnvironmentSetup& a, const EnvironmentSetup& b) noexcept;

/// The environment the views use in a scene with areas: the eye's area's (AreaEnvironment::over() the scene's), eased
/// over kAreaEnvironmentEaseSeconds when the eye changes areas (smoothstep of the time since the change, from wherever
/// the previous ease stood), instant under reduceMotion. Only the area terms ease (exposure, ambient, fill); the rest
/// is the target's at once. Advanced by the caller's frame time, so a run with a fixed step eases alike every time.
class EnvironmentEase {
public:
    EnvironmentEase() = default;
    explicit EnvironmentEase(const EnvironmentSetup& start) : m_from(start), m_to(start), m_current(start) {}

    /// The environment the views use now.
    [[nodiscard]] const EnvironmentSetup& current() const noexcept { return m_current; }
    /// The environment it eases to.
    [[nodiscard]] const EnvironmentSetup& target() const noexcept { return m_to; }
    /// True while an ease is under way.
    [[nodiscard]] bool easing() const noexcept { return m_progress < 1.0f; }

    /// Eases from current() to `target` (instantly with `instant`, or when nothing differs but the non-eased terms).
    void retarget(const EnvironmentSetup& target, bool instant);
    /// current() = target() = `environment` at once (an edit, a new scene).
    void snap(const EnvironmentSetup& environment);
    /// Moves the ease on by `seconds` (ignored when not finite or negative); true when current() changed.
    bool advance(float seconds);

private:
    EnvironmentSetup m_from;
    EnvironmentSetup m_to;
    EnvironmentSetup m_current;
    float m_progress = 1.0f;
};

} // namespace rv::scene
