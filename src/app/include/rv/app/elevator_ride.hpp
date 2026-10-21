#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// The elevator ride (I4 step 7b; ADR-0013 §10, §13): the walker goes from the floor shown to another, the one floor
// resident giving way to the next behind a fade. app::WorldSession drives it frame by frame:
//
//   FadingOut      the view fades out in RideSettings::fadeSeconds (instant under reduceMotion); the destination's
//                  floor-build job runs meanwhile (asked with the ride);
//   AwaitingFloor  faded out: once the job answered, Swap (the session drops the resident floor and shows the
//                  destination at that frame boundary, the walker at its elevator or a room's doorway: swapped()); a
//                  job that failed is Fail (the walker stays where it was, the view fades back in, a notice);
//   AwaitingView   the destination shows behind the fade until the eye's area is resident (its layers in), or for
//                  RideSettings::viewTimeoutSeconds (under lockstep the frames' waits replace the timeout);
//   FadingIn       the view fades back in; Finish once it is back (the ride's record complete);
//   Done.
//
// Input is suspended for the whole ride. Times come from the session (the steady clock since the request), so the
// machine is pure and its unit tests drive it frame by frame.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rv/app/run_report.hpp>

namespace rv::app {

/// Why the walker rides (RideRecord::reason, TeleportRecord::reason).
enum class RideReason : std::uint8_t {
    Floor,      ///< the directory's floor: its elevator
    Room,       ///< the directory's room: its doorway
    ShowInRoom, ///< "Mostra nella stanza": the room's doorway, then the game's Focus pose
};

/// "floor", "room", "showInRoom".
[[nodiscard]] std::string_view toString(RideReason reason) noexcept;

enum class RidePhase : std::uint8_t { FadingOut, AwaitingFloor, AwaitingView, FadingIn, Done };

/// "fading-out", "awaiting-floor", "awaiting-view", "fading-in", "done".
[[nodiscard]] std::string_view toString(RidePhase phase) noexcept;

/// Where a ride goes and why.
struct RideRequest {
    /// The floor shown and the destination floor (building.json's ids).
    std::string from;
    std::string to;
    RideReason reason = RideReason::Floor;
    /// The room whose doorway the walker arrives at (reasons Room and ShowInRoom), the game to show (ShowInRoom).
    std::string room;
    std::string game;
};

struct RideSettings {
    /// Each fade, out and in (ADR-0013 §10: 250 ms).
    double fadeSeconds = 0.25;
    /// How long the destination waits behind the fade for the eye's area to be resident (ADR-0013 §10: 10 s).
    double viewTimeoutSeconds = 10.0;
    /// The world's reduceMotion: both fades instant.
    bool reduceMotion = false;
    /// A lockstep run (--fixed-dt): the frames wait for the floor and the view's layers, no timeout.
    bool lockstep = false;
};

/// One frame of the ride (or an idle wake of a lockstep run, which moves no fade).
struct RideFrame {
    std::uint64_t frame = 0;
    /// The frame time in seconds (the fixed one under --fixed-dt); 0 for an idle wake.
    double deltaSeconds = 0.0;
    /// Milliseconds since the ride was asked.
    double elapsedMs = 0.0;
    /// The eye's area is resident on the destination (read once swapped).
    bool viewResident = false;
};

/// What the session does after an update.
enum class RideAction : std::uint8_t {
    None,
    Swap,   ///< faded out with the destination built: show it now, then swapped()
    Fail,   ///< the destination could not be built: the walker stays, the view fades back in, a notice
    Finish, ///< the view is back: the ride is done (its record complete)
};

class ElevatorRide {
public:
    /// A ride asked at `frame` (RideRecord::requestedFrame), its record "running".
    ElevatorRide(RideRequest request, RideSettings settings, std::uint64_t frame);

    /// The destination's floor-build job answered at `elapsedMs`: built (`built`) or not; `jobMs` its own time.
    void floorAnswered(bool built, double elapsedMs, std::optional<double> jobMs);
    /// One frame (see the file comment).
    [[nodiscard]] RideAction update(const RideFrame& frame);
    /// The session showed the destination at `frame` (after Swap).
    void swapped(std::uint64_t frame);

    /// How dark the view is: 0 not at all, 1 black.
    [[nodiscard]] float fade() const noexcept { return static_cast<float>(m_fade); }
    [[nodiscard]] RidePhase phase() const noexcept { return m_phase; }
    [[nodiscard]] bool done() const noexcept { return m_phase == RidePhase::Done; }
    /// Faded out and the destination not answered yet (a lockstep run waits for it between frames).
    [[nodiscard]] bool awaitsFloor() const noexcept { return m_phase == RidePhase::AwaitingFloor && !m_built && !m_failed; }
    /// The floor was swapped (the destination shows).
    [[nodiscard]] bool hasSwapped() const noexcept { return m_record.swappedFrame.has_value(); }
    [[nodiscard]] bool failed() const noexcept { return m_failed; }
    [[nodiscard]] const RideRequest& request() const noexcept { return m_request; }
    /// The record so far ("running" until Finish); the session adds the walker and the GPU memory at the end.
    [[nodiscard]] const RideRecord& record() const noexcept { return m_record; }
    [[nodiscard]] RideRecord& record() noexcept { return m_record; }

private:
    [[nodiscard]] double step(double deltaSeconds) const noexcept;

    RideRequest m_request;
    RideSettings m_settings;
    RidePhase m_phase = RidePhase::FadingOut;
    double m_fade = 0.0;
    bool m_built = false;
    bool m_failed = false;
    bool m_timedOut = false;
    std::optional<double> m_swapElapsedMs;
    RideRecord m_record;
};

} // namespace rv::app
