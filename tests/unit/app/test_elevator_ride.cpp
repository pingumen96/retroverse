// The elevator ride's state machine (I4 step 7b; ADR-0013 §10; rv/app/elevator_ride.hpp): the fade out in 250 ms (15
// frames at the fixed step), the swap only once faded out with the destination built, always from the frame after the
// fade ended (whenever the job answered), the eye's area awaited, the fade in, the record's frames and times; a
// destination that cannot be built (the walker stays, the view comes back), the view's deadline outside lockstep (the
// view back within 3 s of the request since I4 step 8), none under it, reduceMotion's instant fades, frames without time
// that move no fade.

#include <cstdint>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <rv/app/elevator_ride.hpp>

using rv::app::ElevatorRide;
using rv::app::RideAction;
using rv::app::RideFrame;
using rv::app::RidePhase;
using rv::app::RideReason;
using rv::app::RideRequest;
using rv::app::RideSettings;

namespace {

constexpr double kStep = 0.016667;

RideRequest request(RideReason reason = RideReason::Floor) {
    RideRequest ride;
    ride.from = "ground";
    ride.to = "gen4";
    ride.reason = reason;
    if (reason != RideReason::Floor) {
        ride.room = "room:snes";
    }
    if (reason == RideReason::ShowInRoom) {
        ride.game = "g0000000000000001";
    }
    return ride;
}

/// A frame of the ride: `frame`, the fixed step, `elapsedMs`, the view resident or not.
RideFrame at(std::uint64_t frame, double elapsedMs, bool resident = false, double step = kStep) {
    RideFrame ride;
    ride.frame = frame;
    ride.deltaSeconds = step;
    ride.elapsedMs = elapsedMs;
    ride.viewResident = resident;
    return ride;
}

/// A lockstep run's settings (the fades by the frames' times, no deadline).
RideSettings lockstep() {
    RideSettings settings;
    settings.lockstep = true;
    return settings;
}

} // namespace

TEST_CASE("app/elevator_ride: fade out, swap, the eye's area, fade in", "[app][elevator_ride]") {
    ElevatorRide ride(request(RideReason::Room), lockstep(), 100);
    CHECK(ride.record().from == "ground");
    CHECK(ride.record().to == "gen4");
    CHECK(ride.record().reason == "room");
    CHECK(ride.record().room == "room:snes");
    CHECK(ride.record().requestedFrame == 100);
    CHECK(ride.record().outcome == "running");
    CHECK(ride.phase() == RidePhase::FadingOut);
    CHECK(ride.fade() == 0.0f);

    // The destination built early: nothing swaps before the view is black.
    ride.floorAnswered(true, 40.0, 35.0);
    std::uint64_t frame = 100;
    for (; frame < 114; ++frame) {
        CHECK(ride.update(at(frame, 50.0)) == RideAction::None);
        CHECK(ride.fade() > 0.0f);
        CHECK(ride.fade() < 1.0f);
    }
    // The 15th frame of 0.016667 s completes the 250 ms: black; the floor swaps in the next frame (as it would had the
    // job answered only now).
    CHECK(ride.update(at(frame, 260.0)) == RideAction::None);
    CHECK(ride.fade() == 1.0f);
    CHECK(ride.phase() == RidePhase::AwaitingFloor);
    CHECK(ride.record().fadedOutFrame == std::optional<std::uint64_t>(114));
    CHECK(ride.record().builtMs == std::optional<double>(40.0));
    CHECK(ride.record().jobMs == std::optional<double>(35.0));
    CHECK(ride.update(at(115, 280.0)) == RideAction::Swap);
    ride.swapped(115);
    CHECK(ride.hasSwapped());
    CHECK(ride.phase() == RidePhase::AwaitingView);
    CHECK(ride.record().swappedFrame == std::optional<std::uint64_t>(115));

    // The view stays black until the eye's area is resident.
    CHECK(ride.update(at(116, 300.0, false)) == RideAction::None);
    CHECK(ride.fade() == 1.0f);
    CHECK(ride.update(at(117, 420.0, true)) == RideAction::None);
    CHECK(ride.record().residentMs == std::optional<double>(420.0));
    CHECK(ride.phase() == RidePhase::FadingIn);
    // 15 frames to fade back in.
    frame = 118;
    for (; frame < 132; ++frame) {
        CHECK(ride.update(at(frame, 500.0)) == RideAction::None);
        CHECK(ride.fade() > 0.0f);
    }
    CHECK(ride.update(at(frame, 700.0)) == RideAction::Finish);
    CHECK(ride.done());
    CHECK(ride.fade() == 0.0f);
    CHECK(ride.record().fadedInFrame == std::optional<std::uint64_t>(132));
    CHECK(ride.record().totalMs == std::optional<double>(700.0));
    CHECK(ride.record().outcome == "arrived");
    // Done: nothing more.
    CHECK(ride.update(at(133, 710.0)) == RideAction::None);
}

TEST_CASE("app/elevator_ride: faded out, the ride waits for its floor; a frame without time moves no fade",
          "[app][elevator_ride]") {
    ElevatorRide ride(request(), lockstep(), 0);
    // A frame without time leaves the fade where it is.
    CHECK(ride.update(at(0, 1.0, false, 0.0)) == RideAction::None);
    CHECK(ride.fade() == 0.0f);
    for (std::uint64_t frame = 0; frame < 15; ++frame) {
        CHECK(ride.update(at(frame, 10.0)) == RideAction::None);
    }
    CHECK(ride.phase() == RidePhase::AwaitingFloor);
    CHECK(ride.record().fadedOutFrame == std::optional<std::uint64_t>(14));
    CHECK(ride.awaitsFloor());
    CHECK(ride.update(at(15, 20.0)) == RideAction::None);
    // The floor's job answers in a lockstep wait (the session only hands the answer over there): the swap in the next
    // frame, the one it would have swapped in had the job answered before the fade ended.
    ride.floorAnswered(true, 900.0, 880.0);
    CHECK_FALSE(ride.awaitsFloor());
    CHECK(ride.phase() == RidePhase::AwaitingFloor);
    CHECK(ride.update(at(16, 905.0)) == RideAction::Swap);
    ride.swapped(16);
    CHECK(ride.record().swappedFrame == std::optional<std::uint64_t>(16));
    // A second answer (a stale one) changes nothing.
    ride.floorAnswered(false, 950.0, std::nullopt);
    CHECK_FALSE(ride.failed());
    CHECK(ride.record().builtMs == std::optional<double>(900.0));
}

TEST_CASE("app/elevator_ride: a destination that cannot be built leaves the walker where it was", "[app][elevator_ride]") {
    SECTION("while the view fades out: it comes back from where its fade stands") {
        ElevatorRide ride(request(), lockstep(), 10);
        for (std::uint64_t frame = 10; frame < 15; ++frame) {
            CHECK(ride.update(at(frame, 5.0)) == RideAction::None);
        }
        const float faded = ride.fade();
        CHECK(faded > 0.0f);
        ride.floorAnswered(false, 60.0, 55.0);
        CHECK(ride.update(at(15, 61.0)) == RideAction::Fail);
        CHECK(ride.failed());
        CHECK(ride.phase() == RidePhase::FadingIn);
        CHECK(ride.fade() == faded);
        std::uint64_t frame = 16;
        RideAction action = RideAction::None;
        for (; frame < 60 && action != RideAction::Finish; ++frame) {
            action = ride.update(at(frame, 100.0));
        }
        CHECK(action == RideAction::Finish);
        CHECK(ride.record().outcome == "failed");
        CHECK_FALSE(ride.hasSwapped());
        CHECK_FALSE(ride.record().fadedOutFrame.has_value());
    }
    SECTION("once faded out") {
        ElevatorRide ride(request(), lockstep(), 10);
        for (std::uint64_t frame = 10; frame < 25; ++frame) {
            CHECK(ride.update(at(frame, 5.0)) == RideAction::None);
        }
        CHECK(ride.awaitsFloor());
        ride.floorAnswered(false, 300.0, std::nullopt);
        CHECK(ride.update(at(25, 301.0)) == RideAction::Fail);
        CHECK(ride.record().fadedOutFrame == std::optional<std::uint64_t>(24));
        RideAction action = RideAction::None;
        for (std::uint64_t frame = 26; frame < 60 && action != RideAction::Finish; ++frame) {
            action = ride.update(at(frame, 400.0));
        }
        CHECK(action == RideAction::Finish);
        CHECK(ride.record().outcome == "failed");
    }
}

namespace {

/// A ride outside lockstep driven at 60 frames a second of the real clock until its floor swapped (asked at 0, its
/// floor built at once); the frame it swapped in.
std::uint64_t rideToSwap(ElevatorRide& ride) {
    ride.floorAnswered(true, 1.0, 1.0);
    std::uint64_t frame = 0;
    double elapsedMs = 0.0;
    while (ride.update(at(frame, elapsedMs)) != RideAction::Swap) {
        ++frame;
        elapsedMs += kStep * 1000.0;
    }
    ride.swapped(frame);
    return frame;
}

} // namespace

TEST_CASE("app/elevator_ride: the view is back within the timeout of the request outside lockstep, waits under it",
          "[app][elevator_ride]") {
    // ADR-0013 §10, §13 as amended at I4 step 8: a ride's view is back within 3 s of the request whatever the covers
    // (the eye's area is the covers in view of the room that holds the eye; the rest stream in afterwards). The wait for
    // it ends at the deadline: the timeout after the request less the fade-in (none under reduceMotion) and the margin,
    // 2.25 s; the 250 ms fade-in then ends by 2.5 s, and a frame that comes late shortens it so that it still ends within
    // the timeout.
    CHECK(RideSettings{}.viewTimeoutSeconds == 3.0);
    for (const bool reduceMotion : {false, true}) {
        RideSettings settings;
        settings.reduceMotion = reduceMotion;
        const double deadline = reduceMotion ? 2500.0 : 2250.0;
        CHECK(ElevatorRide(request(), settings, 0).viewDeadlineMs() == deadline);
    }
    SECTION("frames on time") {
        for (const bool lockstep : {false, true}) {
            CAPTURE(lockstep);
            RideSettings settings;
            settings.lockstep = lockstep;
            ElevatorRide ride(request(), settings, 0);
            std::uint64_t frame = rideToSwap(ride);
            // A slow destination: the covers in view of its eye's room still not resident at 2.2 s.
            CHECK(ride.update(at(++frame, 2000.0)) == RideAction::None);
            CHECK(ride.update(at(++frame, 2249.0)) == RideAction::None);
            CHECK(ride.phase() == RidePhase::AwaitingView);
            // The deadline (outside lockstep): the fade-in starts, the record saying the view timed out.
            CHECK(ride.update(at(++frame, 2250.0)) == RideAction::None);
            CHECK(ride.phase() == (lockstep ? RidePhase::AwaitingView : RidePhase::FadingIn));
            if (lockstep) {
                // Under lockstep the frames wait for the view's covers instead: no timeout, even past the old 10 s.
                CHECK(ride.update(at(++frame, 20000.0)) == RideAction::None);
                CHECK(ride.phase() == RidePhase::AwaitingView);
                continue;
            }
            RideAction action = RideAction::None;
            double elapsedMs = 2250.0;
            while (action != RideAction::Finish) {
                elapsedMs += kStep * 1000.0;
                action = ride.update(at(++frame, elapsedMs));
            }
            CHECK(ride.record().outcome == "timed-out");
            CHECK_FALSE(ride.record().residentMs.has_value());
            REQUIRE(ride.record().totalMs.has_value());
            CHECK(*ride.record().totalMs >= 2500.0);
            CHECK(*ride.record().totalMs <= 2520.0);
        }
    }
    SECTION("a long frame before the deadline's") {
        // The frame after 2.2 s comes at 2.9 s: the fade-in has 100 ms left, and ends at 3 s.
        ElevatorRide ride(request(), RideSettings{}, 0);
        std::uint64_t frame = rideToSwap(ride);
        CHECK(ride.update(at(++frame, 2200.0)) == RideAction::None);
        CHECK(ride.update(at(++frame, 2900.0, false, 0.1)) == RideAction::None);
        CHECK(ride.phase() == RidePhase::FadingIn);
        CHECK(ride.update(at(++frame, 2950.0)) == RideAction::None);
        CHECK(ride.fade() == 0.5f);
        CHECK(ride.update(at(++frame, 3000.0)) == RideAction::Finish);
        CHECK(ride.record().totalMs == std::optional<double>(3000.0));
        CHECK(ride.record().outcome == "timed-out");
    }
    SECTION("a frame past the timeout") {
        // The frame after 2.2 s comes at 3.05 s: the view back in it, at once.
        ElevatorRide ride(request(), RideSettings{}, 0);
        std::uint64_t frame = rideToSwap(ride);
        CHECK(ride.update(at(++frame, 2200.0)) == RideAction::None);
        CHECK(ride.update(at(++frame, 3050.0, false, 0.1)) == RideAction::Finish);
        CHECK(ride.fade() == 0.0f);
        CHECK(ride.record().totalMs == std::optional<double>(3050.0));
    }
    SECTION("the eye's area resident in a late frame") {
        // Resident at 2.9 s (its frame late): arrived, the fade-in shortened to end within the timeout.
        ElevatorRide ride(request(), RideSettings{}, 0);
        std::uint64_t frame = rideToSwap(ride);
        CHECK(ride.update(at(++frame, 2200.0)) == RideAction::None);
        CHECK(ride.update(at(++frame, 2900.0, true, 0.1)) == RideAction::None);
        CHECK(ride.update(at(++frame, 3000.0)) == RideAction::Finish);
        CHECK(ride.record().outcome == "arrived");
        CHECK(ride.record().residentMs == std::optional<double>(2900.0));
    }
    SECTION("a longer timeout, as RideSettings allows") {
        // The machine takes any timeout: 10 s (I4 step 7b's) waits until 9.25 s.
        RideSettings settings;
        settings.viewTimeoutSeconds = 10.0;
        ElevatorRide ride(request(), settings, 0);
        CHECK(ride.viewDeadlineMs() == 9250.0);
        std::uint64_t frame = rideToSwap(ride);
        CHECK(ride.update(at(++frame, 9249.0)) == RideAction::None);
        CHECK(ride.phase() == RidePhase::AwaitingView);
        CHECK(ride.update(at(++frame, 9250.0)) == RideAction::None);
        CHECK(ride.phase() == RidePhase::FadingIn);
    }
}

TEST_CASE("app/elevator_ride: outside lockstep the fades follow the real clock, long frames included",
          "[app][elevator_ride]") {
    // A frame's time is clamped in a long frame (0.1 s): the fades count the milliseconds since the request instead.
    ElevatorRide ride(request(), RideSettings{}, 0);
    ride.floorAnswered(true, 1.0, 1.0);
    CHECK(ride.update(at(0, 0.0)) == RideAction::None);
    CHECK(ride.fade() == 0.0f);
    CHECK(ride.update(at(1, 125.0, false, 0.1)) == RideAction::None);
    CHECK(ride.fade() == 0.5f);
    // A 300 ms frame (its time clamped to 100 ms): black at 425 ms all the same.
    CHECK(ride.update(at(2, 425.0, false, 0.1)) == RideAction::None);
    CHECK(ride.fade() == 1.0f);
    CHECK(ride.record().fadedOutFrame == std::optional<std::uint64_t>(2));
    CHECK(ride.update(at(3, 430.0)) == RideAction::Swap);
    ride.swapped(3);
    CHECK(ride.update(at(4, 600.0, true)) == RideAction::None);
    CHECK(ride.update(at(5, 700.0, false, 0.1)) == RideAction::None);
    CHECK(ride.fade() == 0.6f);
    CHECK(ride.update(at(6, 850.0, false, 0.1)) == RideAction::Finish);
    CHECK(ride.record().totalMs == std::optional<double>(850.0));
    CHECK(ride.record().fadedInFrame == std::optional<std::uint64_t>(6));
}

TEST_CASE("app/elevator_ride: reduceMotion makes both fades instant", "[app][elevator_ride]") {
    RideSettings settings;
    settings.reduceMotion = true;
    ElevatorRide ride(request(RideReason::ShowInRoom), settings, 7);
    CHECK(ride.record().reason == "showInRoom");
    CHECK(ride.record().game == "g0000000000000001");
    ride.floorAnswered(true, 2.0, 2.0);
    // Black in the frame that asked, the floor swapped in the next.
    CHECK(ride.update(at(7, 3.0)) == RideAction::None);
    CHECK(ride.fade() == 1.0f);
    CHECK(ride.record().fadedOutFrame == std::optional<std::uint64_t>(7));
    CHECK(ride.update(at(8, 5.0)) == RideAction::Swap);
    ride.swapped(8);
    CHECK(ride.update(at(9, 10.0, true)) == RideAction::None);
    CHECK(ride.update(at(10, 20.0)) == RideAction::Finish);
    CHECK(ride.record().fadedInFrame == std::optional<std::uint64_t>(10));
    CHECK(rv::app::toString(RideReason::Floor) == "floor");
    CHECK(rv::app::toString(RideReason::Room) == "room");
    CHECK(rv::app::toString(RidePhase::AwaitingView) == "awaiting-view");
}
