// I3 step 7: the eased pose animator (scene::PoseAnimator, ADR-0012 §3, §4): eased runs on the frame time the caller
// passes, identical for identical frame times, reversed from where they are when interrupted, skippable, instant under
// reduceMotion.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <rv/core/math.hpp>
#include <rv/scene/pose_animator.hpp>

using Catch::Approx;
using rv::core::degreesToRadians;
using rv::scene::Easing;
using rv::scene::EasedProgress;
using rv::scene::PoseAnimator;
using rv::scene::RigidPose;
using rv::scene::WalkerPose;

namespace {

constexpr float kFrame = 0.016667f;

/// Frames until the animator stops, advancing by `dt`; -1 past `limit`.
template <typename Pose>
int framesToSettle(PoseAnimator<Pose>& animator, float dt, int limit = 10000) {
    int frames = 0;
    while (animator.animating()) {
        if (++frames > limit) {
            return -1;
        }
        animator.advance(dt);
    }
    return frames;
}

} // namespace

TEST_CASE("scene/pose_animator: the easing curves", "[scene][pose_animator]") {
    for (const Easing easing : {Easing::Linear, Easing::EaseInOut, Easing::EaseOut}) {
        CHECK(rv::scene::ease(easing, 0.0f) == 0.0f);
        CHECK(rv::scene::ease(easing, 1.0f) == 1.0f);
        CHECK(rv::scene::ease(easing, -1.0f) == 0.0f);
        CHECK(rv::scene::ease(easing, 2.0f) == 1.0f);
        CHECK(rv::scene::ease(easing, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
        // Rising.
        float previous = 0.0f;
        for (int i = 1; i <= 100; ++i) {
            const float value = rv::scene::ease(easing, static_cast<float>(i) / 100.0f);
            CHECK(value > previous);
            previous = value;
        }
    }
    CHECK(rv::scene::ease(Easing::Linear, 0.25f) == 0.25f);
    CHECK(rv::scene::ease(Easing::EaseInOut, 0.5f) == 0.5f);
    CHECK(rv::scene::ease(Easing::EaseInOut, 0.25f) == Approx(0.15625f));
    CHECK(rv::scene::ease(Easing::EaseOut, 0.5f) == Approx(0.875f));
}

TEST_CASE("scene/pose_animator: interpolation reaches both ends exactly", "[scene][pose_animator]") {
    CHECK(rv::scene::interpolatePose(2.0f, 3.0f, 0.0f) == 2.0f);
    CHECK(rv::scene::interpolatePose(2.0f, 3.0f, 1.0f) == 3.0f);
    CHECK(rv::scene::interpolatePose(2.0f, 3.0f, 0.5f) == 2.5f);
    CHECK(rv::scene::interpolatePose(2.0f, 3.0f, std::numeric_limits<float>::quiet_NaN()) == 2.0f);
    const glm::vec3 a(0.1f, 0.2f, 0.3f);
    const glm::vec3 b(-1.7f, 0.9f, 4.1f);
    CHECK(rv::scene::interpolatePose(a, b, 1.0f) == b);
    CHECK(rv::scene::interpolatePose(a, b, 0.0f) == a);

    // A walker's yaw goes the short way round, wrapped to (-pi, pi].
    const WalkerPose from{{0.0f, 0.0f, 3.0f}, degreesToRadians(170.0f), 0.1f};
    const WalkerPose to{{1.0f, 0.0f, 2.0f}, degreesToRadians(-170.0f), -0.3f};
    const WalkerPose half = rv::scene::interpolatePose(from, to, 0.5f);
    CHECK(std::fabs(rv::core::wrapAngleRadians(half.yaw - rv::core::kPi)) < 1e-5f);
    CHECK(half.pitch == Approx(-0.1f));
    CHECK(half.feet.x == Approx(0.5f));
    const WalkerPose end = rv::scene::interpolatePose(from, to, 1.0f);
    CHECK(end.yaw == to.yaw);
    CHECK(end.feet == to.feet);

    // A rigid pose's orientation by slerp along the shorter arc (a quaternion and its negation are one orientation).
    const RigidPose r0{{0.0f, 0.0f, 0.0f}, glm::angleAxis(0.0f, glm::vec3(0.0f, 1.0f, 0.0f))};
    const RigidPose r1{{0.0f, 1.0f, 0.0f}, -glm::angleAxis(degreesToRadians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f))};
    const RigidPose rh = rv::scene::interpolatePose(r0, r1, 0.5f);
    CHECK(glm::angle(rh.orientation) == Approx(degreesToRadians(45.0f)).margin(1e-4));
    CHECK(rh.position.y == Approx(0.5f));
    CHECK(rv::scene::samePose(r1, RigidPose{r1.position, -r1.orientation}));
    CHECK_FALSE(rv::scene::samePose(r0, r1));
}

TEST_CASE("scene/pose_animator: a run lasts its duration on the frame time given", "[scene][pose_animator]") {
    PoseAnimator<float> animator(0.0f, Easing::EaseInOut);
    CHECK_FALSE(animator.animating());
    CHECK(animator.pose() == 0.0f);
    animator.animateTo(1.0f, 0.25f);
    CHECK(animator.animating());
    CHECK(animator.target() == 1.0f);
    CHECK(animator.pose() == 0.0f);
    // 0.25 s at 60 fps: 15 frames of 1/60 s, the 15th arriving exactly.
    CHECK(framesToSettle(animator, kFrame) == 15);
    CHECK(animator.pose() == 1.0f);
    // Halfway in time is halfway in value on a symmetric curve.
    PoseAnimator<float> half(0.0f, Easing::EaseInOut);
    half.animateTo(1.0f, 1.0f);
    half.advance(0.5f);
    CHECK(half.pose() == Approx(0.5f));
    CHECK(half.progress() == Approx(0.5f));
    // Negative and non-finite frame times move nothing.
    half.advance(-1.0f);
    half.advance(std::numeric_limits<float>::infinity());
    CHECK(half.progress() == Approx(0.5f));
}

TEST_CASE("scene/pose_animator: identical frame times give identical animations", "[scene][pose_animator]") {
    // As two runs with --fixed-dt do: every intermediate pose bit-identical.
    const auto run = [](float dt) {
        PoseAnimator<WalkerPose> animator(WalkerPose{{0.0f, 0.0f, 3.2f}, 0.0f, degreesToRadians(-6.0f)});
        std::vector<WalkerPose> poses;
        animator.animateTo(WalkerPose{{-1.2f, 0.0f, -3.1f}, degreesToRadians(35.0f), degreesToRadians(-10.0f)}, 0.35f);
        for (int frame = 0; frame < 30; ++frame) {
            if (frame == 12) {
                // Interrupted by a new target mid-way.
                animator.animateTo(WalkerPose{{-1.0f, 0.0f, -3.0f}, degreesToRadians(20.0f), 0.0f}, 0.35f);
            }
            animator.advance(dt);
            poses.push_back(animator.pose());
        }
        return poses;
    };
    const std::vector<WalkerPose> first = run(kFrame);
    const std::vector<WalkerPose> second = run(kFrame);
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].feet == second[i].feet);
        CHECK(first[i].yaw == second[i].yaw);
        CHECK(first[i].pitch == second[i].pitch);
    }
    // Another frame time gives another sequence (the animation follows the time given, no clock of its own).
    const std::vector<WalkerPose> slower = run(0.5f * kFrame);
    CHECK_FALSE(slower[5].feet == first[5].feet);
}

TEST_CASE("scene/pose_animator: interrupted, it reverses from where it is", "[scene][pose_animator]") {
    // A pull-out (0.25 s) interrupted at 60% slides back (0.2 s a whole way) in 60% of 0.2 s.
    PoseAnimator<float> pull(0.0f, Easing::EaseInOut);
    pull.animateTo(1.0f, 0.25f);
    pull.advance(0.15f);
    REQUIRE(pull.progress() == Approx(0.6f));
    const float before = pull.pose();
    pull.animateTo(0.0f, 0.2f);
    CHECK(pull.pose() == before); // no jump
    CHECK(pull.target() == 0.0f);
    pull.advance(0.06f);
    CHECK(pull.progress() == Approx(0.3f));
    CHECK(pull.pose() < before);
    pull.advance(0.0599f);
    CHECK(pull.animating());
    pull.advance(0.0002f);
    CHECK_FALSE(pull.animating());
    CHECK(pull.pose() == 0.0f);

    // Reversed again before it arrives: on towards the end from where it is.
    pull.animateTo(1.0f, 0.25f);
    pull.advance(0.1f);
    pull.animateTo(0.0f, 0.2f);
    pull.advance(0.04f);
    const float back = pull.pose();
    pull.animateTo(1.0f, 0.25f);
    CHECK(pull.pose() == back);
    CHECK(pull.target() == 1.0f);
    // The way left, 0.8 of a whole run, takes 0.8 of 0.25 s.
    CHECK(pull.progress() == Approx(0.2f));
    const int frames = framesToSettle(pull, 0.001f);
    CHECK(frames >= 199);
    CHECK(frames <= 201);

    // A walker interrupted mid-ease towards a third pose starts a new run from where it stands.
    PoseAnimator<WalkerPose> walker(WalkerPose{{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f});
    walker.animateTo(WalkerPose{{1.0f, 0.0f, 0.0f}, 0.0f, 0.0f}, 0.35f);
    walker.advance(0.1f);
    const WalkerPose mid = walker.pose();
    walker.animateTo(WalkerPose{{0.0f, 0.0f, 1.0f}, 0.0f, 0.0f}, 0.35f);
    CHECK(rv::scene::samePose(walker.pose(), mid));
    CHECK(walker.progress() == 0.0f);
    // Sent back to the first run's start, which is not this run's: a new run again, from where it stands.
    walker.advance(0.1f);
    const WalkerPose later = walker.pose();
    walker.animateTo(WalkerPose{{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f}, 0.35f);
    CHECK(walker.progress() == 0.0f);
    CHECK(rv::scene::samePose(walker.pose(), later));
    walker.advance(0.36f);
    CHECK_FALSE(walker.animating());
    CHECK(walker.pose().feet == glm::vec3(0.0f, 0.0f, 0.0f));
}

TEST_CASE("scene/pose_animator: skipped, it arrives at once; a target where it stands settles", "[scene][pose_animator]") {
    PoseAnimator<float> lift(0.0f);
    lift.animateTo(1.0f, 0.3f);
    lift.advance(kFrame);
    lift.finish();
    CHECK_FALSE(lift.animating());
    CHECK(lift.pose() == 1.0f);
    // Asked to go where it stands: nothing to animate.
    lift.animateTo(1.0f, 0.3f);
    CHECK_FALSE(lift.animating());
    PoseAnimator<WalkerPose> walker(WalkerPose{{1.0f, 0.0f, 2.0f}, 0.5f, 0.1f});
    walker.animateTo(WalkerPose{{1.0f, 0.0f, 2.0f}, 0.5f + rv::core::kTwoPi, 0.1f}, 0.35f);
    CHECK_FALSE(walker.animating());
    // A duration of 0 or not finite arrives at once.
    lift.animateTo(0.0f, 0.0f);
    CHECK(lift.pose() == 0.0f);
    lift.animateTo(1.0f, std::numeric_limits<float>::quiet_NaN());
    CHECK(lift.pose() == 1.0f);
    // jumpTo stands still at once.
    lift.jumpTo(0.25f);
    CHECK_FALSE(lift.animating());
    CHECK(lift.pose() == 0.25f);
}

TEST_CASE("scene/pose_animator: reduceMotion makes every run instant", "[scene][pose_animator]") {
    PoseAnimator<float> pull(0.0f);
    pull.animateTo(1.0f, 0.25f);
    pull.advance(kFrame);
    CHECK(pull.animating());
    pull.setReduceMotion(true);
    CHECK(pull.reduceMotion());
    CHECK_FALSE(pull.animating());
    CHECK(pull.pose() == 1.0f);
    pull.animateTo(0.0f, 0.2f);
    CHECK_FALSE(pull.animating());
    CHECK(pull.pose() == 0.0f);
    pull.setReduceMotion(false);
    pull.animateTo(1.0f, 0.25f);
    CHECK(pull.animating());

    EasedProgress progress;
    progress.setReduceMotion(true);
    progress.moveTo(1.0f, 5.0f);
    CHECK(progress.value() == 1.0f);
    CHECK_FALSE(progress.moving());
    // Goals snap to 0 or 1.
    progress.setReduceMotion(false);
    progress.moveTo(0.3f, 1.0f);
    CHECK(progress.goal() == 0.0f);
    progress.moveTo(0.7f, 1.0f);
    CHECK(progress.goal() == 1.0f);
}

TEST_CASE("scene/pose_animator: a new curve applies to the runs that start after it, never to one under way",
          "[scene][pose_animator]") {
    PoseAnimator<float> look(0.0f);
    CHECK(look.easing() == Easing::EaseInOut);
    // At rest the curve changes at once (its ends map to themselves: the pose does not move).
    look.setEasing(Easing::EaseOut);
    CHECK(look.easing() == Easing::EaseOut);
    CHECK(look.pose() == 0.0f);
    look.animateTo(1.0f, 0.3f);
    look.advance(0.1f);
    CHECK(look.pose() == Approx(rv::scene::ease(Easing::EaseOut, look.progress())));
    // Asked for during the run: the run keeps its curve (no jump), the next run takes the new one.
    const float before = look.pose();
    look.setEasing(Easing::EaseInOut);
    CHECK(look.pose() == before);
    CHECK(look.easing() == Easing::EaseOut);
    look.advance(1.0f);
    CHECK_FALSE(look.animating());
    CHECK(look.easing() == Easing::EaseInOut);
    // Back from 1 to 0 (the run reversed): its time fraction runs down on the new curve.
    look.animateTo(0.0f, 0.3f);
    look.advance(0.1f);
    CHECK(look.progress() < 1.0f);
    CHECK(look.pose() == Approx(rv::scene::ease(Easing::EaseInOut, look.progress())));
}
