#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// Which arcade cabinets play their own clips (I5 step 4, ADR-0016 §6). A cabinet's screen is a collection item (the
// arcade room's family, ADR-0014 §6) that shows its game's cover; with a decoder slot it plays its game's clip instead:
//  - the focused cabinet (the `focus` class) or the inspected one (`direct`: its clip plays in the hand, the preview
//    linked to its object, style §15.3), while video.json's cabinet showOnFocus holds;
//  - in attract (`ambient`), with nothing focused, the nearest cabinet in view within kAttractMeters whose screen faces
//    the eye, once it has been so for kAttractDwellSeconds (CabinetAttract), while video.json's cabinet enabled holds;
//  - none while the room editor runs (ADR-0015 §4's session, the state Edit): its fixtures and games move, are held and
//    have their room's static part rebuilt, so every cabinet shows its cover until the session ends, and attract starts
//    again from its dwell.
// The arcade's hero cabinet is no collection item: it keeps the room's playlist and never follows the selection (the
// screen service's ScreenBinding::followsSelection).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

#include <rv/app/screen_scheduler.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/world/pick_index.hpp>

namespace rv::app {

/// Attract takes a cabinet whose screen's centre is within this distance of the eye (meters).
inline constexpr double kAttractMeters = 3.0;
/// The cabinet playing attract keeps it while within this distance (the hysteresis: walking along a row does not flip
/// it back and forth at the limit).
inline constexpr double kAttractLeaveMeters = 3.5;
/// A screen faces the eye when the direction from it to the eye is within 60 degrees of its normal; the cabinet playing
/// attract keeps it within 70 degrees.
inline constexpr double kAttractFacingCosine = 0.5;
inline constexpr double kAttractLeaveCosine = 0.342;
/// A cabinet plays attract once it has been the nearest for this long (seconds of the run's simulated time).
inline constexpr double kAttractDwellSeconds = 1.0;

/// A cabinet's screen seen from the eye on one frame.
struct CabinetCandidate {
    world::PickId pick = world::kNoPick;
    /// The screen's centre and its normal (unit, out of the screen), in the scene's frame.
    glm::vec3 center{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    /// Its box meets the view frustum.
    bool visible = false;
};

/// The candidate attract would take from `eye`: the nearest in view within kAttractMeters whose screen faces the eye
/// (`current`, the one playing attract, within kAttractLeaveMeters and kAttractLeaveCosine); equal distances go to the
/// smaller pick id. None when no candidate qualifies.
[[nodiscard]] std::optional<world::PickId> nearestAttract(std::span<const CabinetCandidate> candidates,
                                                          const glm::vec3& eye,
                                                          world::PickId current = world::kNoPick);

/// The cabinet playing attract, frame by frame: a cabinet becomes it once it has been nearestAttract() for
/// kAttractDwellSeconds; the one playing keeps it while it still qualifies (with its hysteresis) and no other has been
/// the nearest for the dwell; it stops at once when it no longer qualifies.
class CabinetAttract {
public:
    /// One frame at `nowSeconds` (the run's simulated time), the eye at `eye`: the cabinet playing attract now.
    std::optional<world::PickId> update(std::span<const CabinetCandidate> candidates, const glm::vec3& eye,
                                        double nowSeconds);
    /// Nothing plays, no dwell runs (Focus, Inspect, the editor, another room).
    void reset() noexcept;

    [[nodiscard]] std::optional<world::PickId> playing() const noexcept { return m_playing; }
    /// The cabinet whose dwell runs, and since when.
    [[nodiscard]] std::optional<world::PickId> candidate() const noexcept { return m_candidate; }

private:
    std::optional<world::PickId> m_playing;
    std::optional<world::PickId> m_candidate;
    double m_since = 0.0;
};

/// What decides the cabinets that play on a frame.
struct CabinetChoice {
    /// The interaction's state and its focused item (Focus, Inspect and a launch's states), kNoPick for none.
    WorldState state = WorldState::FreeRoam;
    world::PickId focused = world::kNoPick;
    /// The room editor runs (its session, whatever the interaction's state).
    bool editing = false;
    /// video.json's cabinet showOnFocus (the focused or inspected cabinet plays) and enabled (attract).
    bool showOnFocus = true;
    bool attract = true;
    /// The cabinet playing attract (CabinetAttract::update()), if any.
    std::optional<world::PickId> attractPick;
};

/// A cabinet that asks for its clip, with its class.
struct CabinetPlay {
    world::PickId pick = world::kNoPick;
    ScreenClass screenClass = ScreenClass::Focus;

    bool operator==(const CabinetPlay&) const = default;
};

/// The cabinets that ask for their clips (see the file comment): the focused one in Focus (`focus`), in Inspect and
/// while its launch runs (`direct`, so that its clip's position is kept for the return); the attract one in free roam
/// (`ambient`); none in the editor.
[[nodiscard]] std::vector<CabinetPlay> chooseCabinets(const CabinetChoice& choice);

/// The scheduler's id of a cabinet's screen (its owner too): "<room id>/cabinet:<pick id as 8 hex digits>", stable while
/// the item stands.
[[nodiscard]] std::string cabinetScreenId(std::string_view room, world::PickId pick);

} // namespace rv::app
