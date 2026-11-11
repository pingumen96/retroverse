#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL, FFmpeg or miniaudio header is included.
//
// The sound of the rooms as the application decides it (I12 step 7, ADR-0021 §5): the room tone's emitters a scene's
// "sound" asks for (the hero TV's hum and hiss, the sign's buzz, a murmur and a clock at the room's centre), each one's
// gain and pan for the eye; the walker's steps (a cadence from the distance walked, the sound by the floor under the
// feet); the clack, the case and the click of the interaction's edges.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/app/world_interaction.hpp>
#include <rv/audio/synth.hpp>
#include <rv/core/aabb.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::app {

/// An emitter is heard whole within this distance of the eye, then as kRoomToneFullMeters / d, never below
/// kRoomToneFloor anywhere in its room (the room's reverberant field: a room's tone fills it).
inline constexpr double kRoomToneFullMeters = 2.0;
inline constexpr double kRoomToneFloor = 0.25;
/// The room tone while a clip is heard.
inline constexpr double kRoomToneUnderClipDecibels = -6.0;
/// The level of each loop at full gain (its buffer's peak is audio::kSynthPeak), before its pack's gain.
inline constexpr double kHumLevel = 0.10;
inline constexpr double kHissLevel = 0.05;
inline constexpr double kBuzzLevel = 0.06;
inline constexpr double kMurmurLevel = 0.12;
inline constexpr double kClockLevel = 0.10;
/// The steps: a stride walking and sprinting (faster than kSprintMetersPerSecond), their level, lower in VR.
inline constexpr double kWalkStrideMeters = 0.72;
inline constexpr double kSprintStrideMeters = 0.95;
inline constexpr double kSprintMetersPerSecond = 2.4;
inline constexpr double kFootstepLevel = 0.35;
inline constexpr double kFootstepVrDecibels = -6.0;
/// A step's pan, left or right foot.
inline constexpr double kFootstepPan = 0.12;
/// More than this in one frame is a teleport, a ride or a reset: no step, the cadence starts again.
inline constexpr double kTeleportMeters = 1.5;
/// The interaction's sounds and the menus' tick.
inline constexpr double kInteractionLevel = 0.5;
inline constexpr double kUiTickLevel = 0.25;
/// The footstep variations the bank holds per surface.
inline constexpr std::uint32_t kFootstepVariants = 4;

/// One loop of a room's tone, where it sounds.
struct RoomToneEmitter {
    audio::SoundId sound = audio::SoundId::CrtHum;
    glm::vec3 position{0.0f};
    /// Its level times its pack's gain (0 never makes an emitter).
    double gain = 0.0;
    /// Heard only while the hero shows a picture (the hiss).
    bool whileShowing = false;
    /// The area it belongs to (none in a scene without areas).
    std::optional<std::size_t> area;
    /// What it stands at: an object's id, or "<area>/centre".
    std::string at;

    bool operator==(const RoomToneEmitter&) const = default;
};

/// The tone of `area` (none: the scene's): its own "sound", else the scene's; none when neither has one.
[[nodiscard]] std::optional<scene::SoundSetup> roomSoundOf(const scene::SceneDescription& description,
                                                           std::optional<std::size_t> area);

/// The emitters of every area (or of the scene without areas) whose tone asks for them: the hum, and the hiss while it
/// shows a picture, 0.25 m over each object whose id ends with "fx:hero.crt"; the buzz at each "fx:hero.sign"; the
/// murmur and the clock 1.6 m over the centre of the area's first box (of the objects' extent without areas). An
/// object's area is the first whose boxes hold it. In the description's order: objects, then centres.
[[nodiscard]] std::vector<RoomToneEmitter> roomToneEmitters(const scene::SceneDescription& description);

/// An emitter as the mixer plays it this frame.
struct RoomToneMix {
    double gain = 0.0;
    double pan = 0.0;
};
/// The share of an emitter's gain heard `distanceMeters` away in its room: 1 within kRoomToneFullMeters, then
/// kRoomToneFullMeters / d, at least kRoomToneFloor.
[[nodiscard]] double roomToneDistanceGain(double distanceMeters) noexcept;
/// `emitter` heard from `eye` looking along `forward`: its gain by distance (roomToneDistanceGain),
/// kRoomToneUnderClipDecibels lower while `clipHeard`, 0 outside `eyeArea` (when it has an
/// area) and for the hiss while the hero shows no picture; its pan by azimuth (audio::panForAzimuth).
[[nodiscard]] RoomToneMix roomToneMix(const RoomToneEmitter& emitter, const glm::vec3& eye, const glm::vec3& forward,
                                      std::optional<std::size_t> eyeArea, bool heroShowing, bool clipHeard) noexcept;

/// The floor under the walker.
enum class FloorSurface : std::uint8_t { Parquet, Rug, Runner };
/// A patch of floor that sounds other than the parquet: a rug's footprint (world space).
struct FloorPatch {
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};
    FloorSurface surface = FloorSurface::Rug;
};
/// The rugs of `description` ("rug" prefabs; the one whose id holds "dressing:runner" a runner) as the footprints of
/// their objects' bounds (`localBounds`, in the description's object order, SceneInstance::objectBounds) moved by their
/// transforms.
[[nodiscard]] std::vector<FloorPatch> floorPatches(const scene::SceneDescription& description,
                                                   std::span<const core::Aabb> localBounds);
/// The surface under `feet` (x, z): the last patch holding it (a rug laid on a runner wins), else parquet.
[[nodiscard]] FloorSurface surfaceUnder(const glm::vec2& feet, std::span<const FloorPatch> patches) noexcept;
/// The step's sound of `surface`.
[[nodiscard]] audio::SoundId footstepSound(FloorSurface surface) noexcept;

/// The cadence of the walker's steps: a step each stride walked (kWalkStrideMeters, kSprintStrideMeters faster than
/// kSprintMetersPerSecond), the first half a stride after the walker sets off; nothing while it does not walk (the
/// interaction eases it, a menu holds it), and a jump past kTeleportMeters starts it again.
class FootstepCadence {
public:
    /// A step this frame: which foot.
    struct Step {
        bool left = false;
    };
    /// The feet at (x, z) after `seconds`; `walking` false holds the cadence (and forgets the last place).
    [[nodiscard]] std::optional<Step> advance(const glm::vec2& feet, double seconds, bool walking) noexcept;
    [[nodiscard]] std::uint64_t steps() const noexcept { return m_steps; }

private:
    std::optional<glm::vec2> m_last;
    double m_travelled = 0.0;
    bool m_left = false;
    std::uint64_t m_steps = 0;
};

/// The interaction's sound when its state goes from `before` to `after`: the clack as an item leaves its shelf (free
/// roam to focus) or goes back (focus to free roam), the case as it opens or closes (focus to or from inspect), the
/// click as it reaches the console (into launching); none otherwise.
[[nodiscard]] std::optional<audio::SoundId> interactionSound(WorldState before, WorldState after) noexcept;

} // namespace rv::app
