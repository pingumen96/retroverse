#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// The elevator ride (I4 step 7b; ADR-0013 §10, §13): the walker goes from the floor shown to another, the one floor
// resident giving way to the next behind a fade. app::WorldSession drives it frame by frame:
//
//   FadingOut      the view fades out in RideSettings::fadeSeconds (instant under reduceMotion; by the frames' times
//                  under lockstep, by the real clock otherwise: a long frame's time is clamped); the destination's
//                  floor-build job runs meanwhile (asked with the ride);
//   AwaitingFloor  faded out (from the frame after the one the fade ended in, so that the frame the floor swaps in is
//                  the same whenever the job answered: under lockstep always the next): once the job answered, Swap (the
//                  session drops the resident floor and shows the destination at that frame boundary, the walker at its
//                  elevator or a room's doorway: swapped()); a job that failed is Fail (the walker stays where it was,
//                  the view fades back in, a notice);
//   AwaitingView   the destination shows behind the fade until the eye's area is resident (its layers in), or until the
//                  view's deadline: RideSettings::viewTimeoutSeconds after the request less the fade-in and a margin
//                  (viewDeadlineMs()), so that the view is back within the timeout of the request (ADR-0013 §13's 10 s
//                  cold); under lockstep the frames' waits replace the deadline;
//   FadingIn       the view fades back in (as the fade out: by frames under lockstep, by the real clock otherwise, a fade
//                  begun late shortened to end within the timeout, one begun past it ended at once); Finish once it is
//                  back (the ride's record complete);
//   Done.
//
// Input is suspended for the whole ride. Times come from the session (the steady clock since the request), so the
// machine is pure and its unit tests drive it frame by frame. The session moves it in frames only: a lockstep run's
// idle wake only gives it the job's answer (floorAnswered()).

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
    /// How long after the request the view is back at the latest, the eye's area resident or not (ADR-0013 §10, §13:
    /// 10 s): the destination waits behind the fade until viewDeadlineMs().
    double viewTimeoutSeconds = 10.0;
    /// The world's reduceMotion: both fades instant.
    bool reduceMotion = false;
    /// A lockstep run (--fixed-dt): the fades by the frames' times, the frames wait for the floor and the view's layers,
    /// no timeout.
    bool lockstep = false;
};

/// The margin the view's deadline leaves before RideSettings::viewTimeoutSeconds besides the fade-in: the frame that
/// sees the deadline passed may come late (the swap's frame and the first uploads into a floor's new arrays take up to
/// half a second on the real library), and the fade-in then shortens to end within the timeout.
inline constexpr double kViewDeadlineMarginSeconds = 0.5;

/// One frame of the ride.
struct RideFrame {
    std::uint64_t frame = 0;
    /// The frame time in seconds (the fixed one under --fixed-dt); 0 moves no fade (under lockstep).
    double deltaSeconds = 0.0;
    /// Milliseconds since the ride was asked (the real clock: the fades outside lockstep, the deadline, the record).
    double elapsedMs = 0.0;
    /// The eye's area is resident on the destination (read once swapped).
    bool viewResident = false;
};

/// What the session does after an update.
enum class RideAction : std::uint8_t {
    None,
    Swap,   ///< faded out (since the last frame) with the destination built: show it now, then swapped()
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
    /// Milliseconds after the request at which the view stops waiting for the eye's area (outside lockstep):
    /// viewTimeoutSeconds less the fade-in (none under reduceMotion) and kViewDeadlineMarginSeconds, at least 0.
    [[nodiscard]] double viewDeadlineMs() const noexcept;

private:
    /// A fade's milliseconds (0 under reduceMotion), the timeout's.
    [[nodiscard]] double fadeMs() const noexcept;
    [[nodiscard]] double timeoutMs() const noexcept;
    /// A lockstep frame's share of a fade.
    [[nodiscard]] double step(double deltaSeconds) const noexcept;
    /// The fade-in from where the fade stands, its length (shortened outside lockstep to end within the timeout).
    void beginFadeIn(const RideFrame& frame);
    /// The view back: the record complete, Finish.
    RideAction finish(const RideFrame& frame);

    RideRequest m_request;
    RideSettings m_settings;
    RidePhase m_phase = RidePhase::FadingOut;
    double m_fade = 0.0;
    /// The fade-in: where the fade stood when it began, when (milliseconds since the request) and how long it takes.
    double m_fadeInFrom = 1.0;
    double m_fadeInStartMs = 0.0;
    double m_fadeInMs = 0.0;
    bool m_built = false;
    bool m_failed = false;
    bool m_timedOut = false;
    RideRecord m_record;
};

} // namespace rv::app
