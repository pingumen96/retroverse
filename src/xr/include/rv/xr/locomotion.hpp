#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/scene/collision.hpp>
#include <rv/xr/pose.hpp>

namespace rv::xr {

/// How the stick turns the user (ADR-0017 §5).
enum class TurnMode : std::uint8_t {
    /// By ComfortSettings::snapDegrees at once, each time the stick is pushed past Locomotion::kSnapPress.
    Snap,
    /// Continuously, ComfortSettings::smoothDegreesPerSecond at full deflection.
    Smooth,
};

/// Where the head's height comes from (ADR-0017 §5).
enum class Posture : std::uint8_t {
    /// The tracked height above the floor.
    Standing,
    /// The tracked height made up to the walker's eye height when the session starts and at each recentering.
    Seated,
};

/// "snap" or "smooth".
[[nodiscard]] std::string_view toString(TurnMode mode) noexcept;
/// "standing" or "seated".
[[nodiscard]] std::string_view toString(Posture posture) noexcept;

/// The comfort options of xr.json (ADR-0017 §5, style §12.4).
struct ComfortSettings {
    TurnMode turn = TurnMode::Snap;
    /// A snap turn, degrees.
    float snapDegrees = 30.0f;
    /// The smooth turn at full deflection, degrees per second.
    float smoothDegreesPerSecond = 90.0f;
    /// [0, 1]: how far the comfort vignette closes in while the stick walks or turns the user; 0 turns it off.
    float vignette = 0.0f;

    bool operator==(const ComfortSettings&) const = default;
};

/// What moves the walker in VR this frame (Locomotion::update): the walker's actions, read by the application.
struct LocomotionInput {
    /// MoveRight and MoveForward, [-1, 1] each, along the head's heading.
    glm::vec2 move{0.0f};
    bool sprint = false;
    /// LookX, [-1, 1]: positive turns right (a snap turn past Locomotion::kSnapPress, or a smooth turn at its rate).
    float turn = 0.0f;
    /// A turn of this frame in radians, positive to the right (the mouse's), in either turn mode.
    float turnRadians = 0.0f;
    /// LookY, [-1, 1]: pushed up past Locomotion::kAimPress, the teleport's arc follows where the head looks; let go
    /// below Locomotion::kAimRelease, the walker lands where it came down.
    float aim = 0.0f;
};

/// The teleport's arc (computeTeleportArc).
struct TeleportArc {
    /// From its start to where it ends: on the floor, against a collider, or at its longest flight.
    std::vector<glm::vec3> points;
    /// Where the walker's feet land: the arc came down on the floor where the capsule fits. None otherwise.
    std::optional<glm::vec3> landing;
};

/// The arc's launch speed (m/s), its gravity (m/s²), its time step and its longest flight (seconds).
inline constexpr float kArcSpeed = 8.0f;
inline constexpr float kArcGravity = 9.81f;
inline constexpr float kArcStepSeconds = 0.025f;
inline constexpr float kArcMaxSeconds = 2.0f;
/// A collider whose top is within this of the floor is the floor itself (a slab, a rug): the arc passes over it.
inline constexpr float kArcFloorTolerance = 0.05f;

/// The ballistic arc from `start` along `direction` at kArcSpeed, sampled every kArcStepSeconds. It ends where it first
/// comes down through the plane y = floorY: a landing when the capsule standing there overlaps no collider (none with no
/// collision world). Or where it first enters a collider standing above the floor (a wall, a shelf, a table: no
/// landing), a turned collider taken as the box around it. Or after kArcMaxSeconds (no landing). A direction that is
/// not finite or is zero, or a start that is not finite, gives no point at all.
[[nodiscard]] TeleportArc computeTeleportArc(const glm::vec3& start, const glm::vec3& direction, float floorY,
                                             const scene::CollisionWorld* collision, const scene::Capsule& capsule);

/// The walker's tuning that the movement in VR shares (scene::FirstPersonSettings), and the comfort options.
struct LocomotionSettings {
    float walkSpeed = 2.5f;
    float sprintSpeed = 4.5f;
    /// The seated posture's head height above the floor.
    float eyeHeight = 1.65f;
    scene::Capsule capsule;
    Posture posture = Posture::Standing;
    ComfortSettings comfort;
};

/// What the movement did so far, for the run report.
struct LocomotionCounters {
    std::uint64_t snapTurns = 0;
    std::uint64_t teleports = 0;
    /// Frames the head leaned further than Locomotion::kMaxHeadOffset beyond the capsule and was pushed back.
    std::uint64_t pushedFrames = 0;
    /// The farthest the head leaned beyond the capsule, meters.
    float largestHeadOffset = 0.0f;
};

/// The walker's movement in VR (ADR-0017 §5). The tracking space is anchored in the world (anchor()); the walker's
/// capsule follows the head over the floor through the collision world, and the stick moves the anchor with it:
///  - the head's pose in the world is the anchor's turn and offset applied to the tracked head;
///  - turning (a snap, or smooth, or the mouse) turns the anchor about the head, which stays where it is;
///  - the stick walks along the head's heading at the walker's speeds, through moveCapsule, carrying the anchor;
///  - the capsule then follows the head (moveCapsule towards the point under it). A head that leans beyond the capsule
///    (over a table, into a wall) fades the view from kFadeStart, black at kMaxHeadOffset, and further than that is
///    pushed back: the anchor moves so that the head stays kMaxHeadOffset beyond the capsule;
///  - the teleport's arc, while aimed, follows the head's gaze from below it; let go over a landing, the anchor moves
///    the head above the landing (a blink: the view black, back in kBlinkSeconds);
///  - the comfort vignette closes in while the stick walks or turns the user smoothly, in kTunnelSeconds.
/// place() puts the walker somewhere else (a spawn, a ride, the world's ease): the next update puts the head above its
/// feet facing its yaw. recenter() follows the runtime's recentering: the head stays where it was. The seated posture
/// makes the head's height up to the walker's eye height at the first update and at each recentering. GPU-free,
/// deterministic; main thread.
class Locomotion {
public:
    /// A snap turn when the stick passes this, again once it went back under kSnapRelease.
    static constexpr float kSnapPress = 0.75f;
    static constexpr float kSnapRelease = 0.35f;
    /// The teleport aims past this, and jumps once the stick went back under kAimRelease.
    static constexpr float kAimPress = 0.7f;
    static constexpr float kAimRelease = 0.35f;
    /// How far (meters, horizontally) the head may lean beyond the capsule before it is pushed back, and where the view
    /// starts fading on the way there.
    static constexpr float kMaxHeadOffset = 0.30f;
    static constexpr float kFadeStart = 0.12f;
    /// The arc starts this far below the head (from the chest, where the eyes see it).
    static constexpr float kArcBelowHead = 0.35f;
    /// The blink of a teleport, and the time the comfort vignette takes to close or open.
    static constexpr float kBlinkSeconds = 0.15f;
    static constexpr float kTunnelSeconds = 0.2f;

    /// Applies settings: speeds and the eye height finite and at least 0, the snap turn within [1, 180] degrees, the
    /// smooth turn within [1, 720] degrees per second, the vignette within [0, 1] (non-finite values take the
    /// defaults).
    void setSettings(const LocomotionSettings& settings) noexcept;
    [[nodiscard]] const LocomotionSettings& settings() const noexcept { return m_settings; }

    /// The next update puts the head above `feet` facing `yawRadians` (scene::Camera's yaw), the capsule at `feet`.
    /// Non-finite values are ignored.
    void place(const glm::vec3& feet, float yawRadians) noexcept;
    /// The next update keeps the head where it is in the world, facing as it does (the runtime moved the space's
    /// origin: XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING), and measures the seated height again.
    void recenter() noexcept;

    /// Advances one frame of `dt` seconds (non-finite or negative counts as 0) with the head tracked at `head` in the
    /// tracking space, whose origin `floorOffset` raises (the runtime's: LOCAL's). A head that is not finite changes
    /// nothing. `collision` null walks through everything.
    void update(const Pose& head, float floorOffset, const LocomotionInput& input, float dt,
                const scene::CollisionWorld* collision);

    /// Where the tracking space sits in the world, its floor offset with the seated posture's height in it.
    [[nodiscard]] TrackingAnchor anchor() const noexcept;
    /// The head in the world after the last update.
    [[nodiscard]] const Pose& head() const noexcept { return m_head; }
    /// The capsule's feet, the head's heading (scene::Camera's yaw) and pitch: the walker's pose.
    [[nodiscard]] const glm::vec3& feet() const noexcept { return m_feet; }
    [[nodiscard]] float heading() const noexcept { return m_heading; }
    [[nodiscard]] float pitch() const noexcept { return m_pitch; }
    /// [0, 1]: the view faded to black (the head beyond the capsule, a teleport's blink).
    [[nodiscard]] float fade() const noexcept;
    /// [0, 1]: the comfort vignette.
    [[nodiscard]] float tunnel() const noexcept { return m_tunnel; }
    /// The teleport's arc while it is aimed.
    [[nodiscard]] const std::optional<TeleportArc>& arc() const noexcept { return m_arc; }
    [[nodiscard]] const LocomotionCounters& counters() const noexcept { return m_counters; }

private:
    struct Placement {
        glm::vec3 feet{0.0f};
        float yaw = 0.0f;
    };

    /// `tracked` (a position in the tracking space) in the world.
    [[nodiscard]] glm::vec3 worldOf(const glm::vec3& tracked) const noexcept;
    /// Moves the anchor horizontally so that `tracked` is at `world` (x, z).
    void anchorAt(const glm::vec2& world, const glm::vec3& tracked) noexcept;

    LocomotionSettings m_settings;
    glm::vec3 m_origin{0.0f};
    float m_yaw = 0.0f;
    float m_floorOffset = 0.0f;
    /// The seated posture's height made up (0 standing).
    float m_heightOffset = 0.0f;
    glm::vec3 m_feet{0.0f};
    Pose m_head;
    float m_heading = 0.0f;
    float m_pitch = 0.0f;
    std::optional<Placement> m_placement;
    bool m_started = false;
    bool m_recenter = false;
    bool m_snapArmed = true;
    bool m_aiming = false;
    std::optional<TeleportArc> m_arc;
    float m_wallFade = 0.0f;
    float m_blink = 0.0f;
    float m_tunnel = 0.0f;
    LocomotionCounters m_counters;
};

} // namespace rv::xr
