#pragma once

// The eased pose animator of the world's interaction (ADR-0012 §3, §4; I3 step 7): a pull-out, the walker's ease to a
// shelf, the lift of an inspected item. Interruptible (a new target reverses it from where it is), skippable (finish()),
// instant under the world's reduceMotion setting, and advanced on the frame time the caller passes, so that a run with
// --fixed-dt animates on the fixed time and two such runs animate identically. GPU-free.

#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <rv/scene/viewpoint.hpp>

namespace rv::scene {

/// The curve a run of an animation follows, from 0 at its start to 1 at its end.
enum class Easing : std::uint8_t {
    Linear,
    EaseInOut, ///< smoothstep: starts and stops gently, alike both ways (every animation of the interaction)
    EaseOut,   ///< cubic: a fast start and a gentle stop
};

/// `t` on `easing`'s curve: 0 and 1 map to themselves exactly, `t` is clamped to [0, 1], a non-finite `t` counts as 0.
[[nodiscard]] float ease(Easing easing, float t) noexcept;

/// A rigid pose: an item held for inspection, pulled out of its row.
struct RigidPose {
    glm::vec3 position{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
};

/// Distances below which two poses are the same pose (PoseAnimator's reversal test): 0.01 mm, 1e-5 radians, and 1e-6
/// for plain values (a progress in [0, 1]).
inline constexpr float kSamePositionTolerance = 1e-5f;
inline constexpr float kSameAngleTolerance = 1e-5f;
inline constexpr float kSameValueTolerance = 1e-6f;

/// The pose `t` of the way from `a` to `b`: `a` at t <= 0 and `b` at t >= 1, exactly. Values and positions are
/// interpolated linearly; a walker's feet linearly, its yaw along the shorter arc (the result wrapped to (-pi, pi]) and
/// its pitch linearly; a rigid pose's orientation by slerp along the shorter arc.
[[nodiscard]] float interpolatePose(float a, float b, float t) noexcept;
[[nodiscard]] glm::vec3 interpolatePose(const glm::vec3& a, const glm::vec3& b, float t) noexcept;
[[nodiscard]] WalkerPose interpolatePose(const WalkerPose& a, const WalkerPose& b, float t) noexcept;
[[nodiscard]] RigidPose interpolatePose(const RigidPose& a, const RigidPose& b, float t) noexcept;

/// True when two poses are the same within the tolerances above (yaws compared as angles, a quaternion and its negation
/// being the same orientation).
[[nodiscard]] bool samePose(float a, float b) noexcept;
[[nodiscard]] bool samePose(const glm::vec3& a, const glm::vec3& b) noexcept;
[[nodiscard]] bool samePose(const WalkerPose& a, const WalkerPose& b) noexcept;
[[nodiscard]] bool samePose(const RigidPose& a, const RigidPose& b) noexcept;

/// The time fraction of a run between two ends, 0 and 1, moving towards one of them at the rate of a whole run's
/// duration. Turned round before it arrives, it goes back from where it is: the way back takes the fraction already
/// travelled of the new duration.
class EasedProgress {
public:
    /// Heads for `goal` (0 or 1: values below 0.5 count as 0, others as 1), at the rate of a whole run of
    /// `fullDurationSeconds`. A duration that is not positive or not finite, or reduceMotion, arrives at once.
    void moveTo(float goal, float fullDurationSeconds) noexcept;
    /// Stands at `value` (clamped to [0, 1]; non-finite counts as 0) and stops.
    void jumpTo(float value) noexcept;
    /// Moves on by `seconds` (non-finite or negative counts as 0), stopping at the goal.
    void advance(float seconds) noexcept;
    /// Arrives at the goal at once (skip).
    void finish() noexcept;
    /// On: finishes now, and every later moveTo arrives at once.
    void setReduceMotion(bool on) noexcept;

    [[nodiscard]] float value() const noexcept { return m_value; }
    [[nodiscard]] float goal() const noexcept { return m_goal; }
    [[nodiscard]] bool moving() const noexcept { return m_value != m_goal; }
    [[nodiscard]] bool reduceMotion() const noexcept { return m_reduceMotion; }

private:
    float m_value = 0.0f;
    float m_goal = 0.0f;
    /// Fraction per second.
    float m_rate = 0.0f;
    bool m_reduceMotion = false;
};

/// An eased animation of a pose between two ends, `Pose` being one of the types interpolatePose() and samePose() take.
///
/// The pose is interpolatePose(from, to, ease(easing, progress)). animateTo(target, duration):
///  - `target` is the start of the current run (and the run has two distinct ends): the run reverses from where it is
///    and takes the fraction it has travelled of `duration` to get back (a pull-out interrupted at 60% slides back in
///    60% of its push-back time);
///  - `target` is the end of the current run: it goes (on, or again) there;
///  - `target` is where the pose stands: it settles there at once;
///  - otherwise a new run starts from the current pose to `target`, lasting `duration`.
/// Nothing is driven by a clock: advance() moves it by the frame time the caller passes.
template <typename Pose>
class PoseAnimator {
public:
    explicit PoseAnimator(const Pose& rest = Pose{}, Easing easing = Easing::EaseInOut) noexcept
        : m_from(rest), m_to(rest), m_easing(easing) {
        m_progress.jumpTo(1.0f);
    }

    /// Stands at `pose` and stops (both ends become `pose`).
    void jumpTo(const Pose& pose) noexcept {
        m_from = pose;
        m_to = pose;
        m_progress.jumpTo(1.0f);
    }

    /// See the class comment.
    void animateTo(const Pose& target, float durationSeconds) noexcept {
        if (!samePose(m_from, m_to) && samePose(target, m_from)) {
            m_progress.moveTo(0.0f, durationSeconds);
            return;
        }
        if (samePose(target, m_to)) {
            m_progress.moveTo(1.0f, durationSeconds);
            return;
        }
        const Pose current = pose();
        if (samePose(target, current)) {
            jumpTo(target);
            return;
        }
        m_from = current;
        m_to = target;
        m_progress.jumpTo(0.0f);
        m_progress.moveTo(1.0f, durationSeconds);
    }

    void advance(float seconds) noexcept { m_progress.advance(seconds); }
    /// Arrives where it is heading at once (skip).
    void finish() noexcept { m_progress.finish(); }
    /// See EasedProgress::setReduceMotion.
    void setReduceMotion(bool on) noexcept { m_progress.setReduceMotion(on); }

    [[nodiscard]] Pose pose() const noexcept {
        return interpolatePose(m_from, m_to, ease(m_easing, m_progress.value()));
    }
    /// Where it is heading (or stands).
    [[nodiscard]] const Pose& target() const noexcept { return m_progress.goal() < 0.5f ? m_from : m_to; }
    [[nodiscard]] bool animating() const noexcept { return m_progress.moving(); }
    /// The time fraction of the current run, 0 at its start and 1 at its end.
    [[nodiscard]] float progress() const noexcept { return m_progress.value(); }
    [[nodiscard]] bool reduceMotion() const noexcept { return m_progress.reduceMotion(); }

private:
    Pose m_from;
    Pose m_to;
    Easing m_easing = Easing::EaseInOut;
    EasedProgress m_progress;
};

} // namespace rv::scene
