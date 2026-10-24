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
// - A view packs (AreaLighting; ADR-0014 §4 as amended on 2026-10-24): the eye's area's directional lights and its
//   key light (its first light with castsShadows), then a view-independent budget of point and spot lights. The
//   neighbours' lights that may be seen through a portal (within kNeighbourReachM plus their own range of it) get a
//   share of the capacity: kNeighbourLightReserve slots (at most half), or all those the area's own lights leave
//   (a hall of 3 lights leaves 13), whichever is more, never more than there are such lights; the area's own lights
//   take the rest, those nearest the eye (one behind the eye's plane counted farther by twice its distance behind
//   it), and the share's slots the neighbours cannot use while their lights lie wholly behind the eye or their portal
//   does (0.5 m behind it, taken back at 0.25 m: a room seen from its doorway, the hall behind), until those come into
//   view. The neighbours' lights take their share by importance: intensity /
//   (1 + d^2), d the path from the eye through the portal to the light, times a factor from 1 for a portal straight
//   ahead down to 0.1 for one beside or behind the eye (a smoothstep of the cosine, from 0 to 0.7). A neighbour's
//   light is never the key light and casts no shadow (gl::packLights' keyCandidates: ownCount()).
// - Stable: a light chosen stays chosen while no light left out is nearer by kOwnLightStickM (own lights) or
//   kNeighbourLightStick times as important (neighbours'), and for kLightDwellSeconds at least whatever the others
//   do; a light left out is not taken back for as long. A point or spot light whose whole range lies behind the eye (its distance behind the plane through the eye
//   across the view at least its range) lights nothing in view: it is left out once it is kBehindEyeMarginM past that
//   and taken back once it is not (the key light stays, for its shadow). So a still or slowly moving eye never
//   toggles a light.
// - Faded: every light packed carries a weight (weights()) that rises from 0 to 1 over kLightFadeSeconds once it is
//   chosen and falls back once it is left out (the smoothstep of a linear progress, moved on by the caller's frame
//   time, so that --fixed-dt runs alike). A light chosen waits for a free slot while the capacity is full of lights
//   fading out (the key light takes the faintest one's). At once (weights 0 or 1) at the start, under reduceMotion,
//   after a teleport (update()'s `instant`) and after an edit (reselect()).
// - Clipped: each light packed may carry a portal (portals(), an axis-aligned rectangle): a neighbour's light the
//   portal its area shares with the eye's nearest to it, a light of an area with a single portal (a room) that
//   portal (a hall with one room alike). The shaders light a fragment on the far side of the portal's plane only along
//   a segment that crosses the rectangle, with a soft edge (common/lights.glsl's portalVisibility), so that no light
//   shines through the wall beside a doorway.
// - Other lights are left out, so that their light neither leaks through walls nor counts against the capacity; the
//   ambient lights every area.
// The work per frame is a pass over the eye's area's lights, its neighbours' and the scene's light states:
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

/// The point and spot lights a view holds back for its area's neighbours from the area's own (at most half the
/// capacity), while the neighbours have as many that may be seen through a portal: within kNeighbourReachM (about a
/// room's depth) of it plus their own range.
inline constexpr std::size_t kNeighbourLightReserve = 6;
inline constexpr float kNeighbourReachM = 10.0f;
/// How long a light packed takes to fade in or out.
inline constexpr float kLightFadeSeconds = 0.3f;
/// The hysteresis of the selection: a light wholly behind the eye is left out this far past its range; an own light
/// chosen stays while no light left out is nearer by this much; a neighbour's while none is this many times as
/// important.
inline constexpr float kBehindEyeMarginM = 1.0f;
inline constexpr float kOwnLightStickM = 1.0f;
inline constexpr float kNeighbourLightStick = 2.0f;
/// A light chosen stays chosen at least this long (while it may be: a candidate of the eye's area), and a light left
/// out stays out as long, so that a light never comes and goes within one fade's time.
inline constexpr float kLightDwellSeconds = 0.5f;
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

/// Which area holds the eye, and the lights a view packs there with their weights and portals (see the file comment).
/// Empty (no areas): it selects nothing and every call changes nothing.
class AreaLighting {
public:
    AreaLighting() = default;
    /// Starts in the area nearest `start` with the lights a view from there packs, at their full weight. `capacity` is
    /// the renderer's number of point and spot lights (gl::kMaxPunctualLights).
    AreaLighting(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& start,
                 std::size_t capacity);

    /// True for a scene without areas.
    [[nodiscard]] bool empty() const noexcept { return m_areaLights.empty(); }
    /// The index of the area that holds the eye; nothing without areas.
    [[nodiscard]] std::optional<std::size_t> area() const noexcept { return m_area; }
    /// The lights a view packs, as indices into the scene's lights: the eye's area's own, then the others (its
    /// neighbours', and lights still fading out), each part in the scene's order; at most the capacity of point and
    /// spot lights.
    [[nodiscard]] std::span<const std::uint32_t> selection() const noexcept { return m_selection; }
    /// The weight of each light of selection(), 0 to 1: its intensity is scaled by it.
    [[nodiscard]] std::span<const float> weights() const noexcept { return m_weights; }
    /// The portal of each light of selection(); an empty box for none.
    [[nodiscard]] std::span<const core::Aabb> portals() const noexcept { return m_portals; }
    /// How many of selection()'s first lights are the eye's area's own.
    [[nodiscard]] std::size_t ownCount() const noexcept { return m_ownCount; }
    /// The lights chosen (packed at their full weight once faded in), as indices into the scene's lights, sorted.
    [[nodiscard]] std::vector<std::uint32_t> chosen() const;
    /// The eye's area's key light (its first light with castsShadows) when it is packed; nothing otherwise.
    [[nodiscard]] std::optional<std::uint32_t> keyLight() const noexcept { return m_key; }
    /// True while a light fades in or out or waits for a slot.
    [[nodiscard]] bool fading() const noexcept { return m_fading; }
    /// The neighbours of each area (areaNeighbours()).
    [[nodiscard]] const std::vector<std::vector<AreaNeighbour>>& neighbours() const noexcept { return m_neighbours; }

    /// Follows the eye to `eye` with the scene's lights as they are now (`lights`, the list this was made with, perhaps
    /// edited in place): the area that holds it and the lights chosen there, `forward` the view's unit direction (zero:
    /// none, every light counts as in front), the fades moved on by `seconds` (or done at once with `instant`). True
    /// when the selection, a weight or a portal changed.
    bool update(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& eye,
                const glm::vec3& forward, float seconds, bool instant = false);
    /// update() at once (no fade).
    bool update(std::span<const SceneArea> areas, std::span<const Light> lights, const glm::vec3& eye,
                const glm::vec3& forward = glm::vec3(0.0f));
    /// The lights chosen for the current area again, with `lights` as they are now (after an edit of their positions or
    /// flags), at once; true when the selection changed.
    bool reselect(std::span<const Light> lights, const glm::vec3& eye, const glm::vec3& forward = glm::vec3(0.0f));

private:
    struct LightState {
        float progress = 0.0f; ///< of the fade, 0 to 1 (the weight is its smoothstep)
        bool packed = false;
        bool chosen = false;
        bool wasChosen = false; ///< chosen at the update before (choose()'s hysteresis)
        float chosenSeconds = 0.0f; ///< how long it has been chosen (kLightDwellSeconds)
        float leftSeconds = 1e9f;   ///< how long since it was last left out after being chosen (kLightDwellSeconds)
        bool behind = false;    ///< wholly behind the eye (with hysteresis)
        bool portalBehind = false; ///< a neighbour's light whose portal lies behind the eye (with hysteresis)
    };

    /// Chooses the lights for the eye; `dwell`: the lights chosen less than kLightDwellSeconds ago stay (not at a cut).
    void choose(std::span<const Light> lights, const glm::vec3& eye, const glm::vec3& forward, bool dwell);
    /// Moves the fades on by `step` (a share of a whole fade; 1 or more: at once) and builds the selection; true when
    /// it changed.
    bool advance(std::span<const Light> lights, float step);
    /// Whether the light at `index` is wholly behind the eye now (its state's hysteresis updated).
    [[nodiscard]] bool behindEye(std::size_t index, const Light& light, const glm::vec3& eye,
                                 const glm::vec3& forward) noexcept;

    std::vector<std::vector<std::uint32_t>> m_areaLights;
    std::vector<std::vector<AreaNeighbour>> m_neighbours;
    /// The area of each light, an index into the areas (past them: none).
    std::vector<std::size_t> m_lightArea;
    std::vector<LightState> m_states;
    std::size_t m_capacity = 0;
    std::optional<std::size_t> m_area;
    std::optional<std::uint32_t> m_key;
    bool m_fading = false;
    std::vector<std::uint32_t> m_selection;
    std::vector<float> m_weights;
    std::vector<core::Aabb> m_portals;
    std::size_t m_ownCount = 0;
    /// Scratch of choose() and advance(), kept to spare allocations per frame: the point and spot lights chosen, the
    /// most important first; the candidates; the selection before.
    std::vector<std::uint32_t> m_priority;
    std::vector<std::pair<float, std::uint32_t>> m_candidates;
    std::vector<std::uint32_t> m_neighbourPicks;
    std::vector<std::uint32_t> m_previous;
    std::vector<float> m_previousWeights;
    std::vector<core::Aabb> m_previousPortals;
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
