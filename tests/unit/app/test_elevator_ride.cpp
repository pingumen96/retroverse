// The elevator ride's state machine (I4 step 7b; ADR-0013 §10; rv/app/elevator_ride.hpp): the fade out in 250 ms (15
// frames at the fixed step), the swap only once faded out with the destination built, the eye's area awaited, the fade
// in, the record's frames and times; a destination that cannot be built (the walker stays, the view comes back), the
// view's timeout outside lockstep, none under it, reduceMotion's instant fades, idle wakes that move no fade.

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

} // namespace

TEST_CASE("app/elevator_ride: fade out, swap, the eye's area, fade in", "[app][elevator_ride]") {
    ElevatorRide ride(request(RideReason::Room), RideSettings{}, 100);
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
    // The 15th frame of 0.016667 s completes the 250 ms: black, and the floor swaps in that frame.
    CHECK(ride.update(at(frame, 260.0)) == RideAction::Swap);
    CHECK(ride.fade() == 1.0f);
    CHECK(ride.record().fadedOutFrame == std::optional<std::uint64_t>(114));
    CHECK(ride.record().builtMs == std::optional<double>(40.0));
    CHECK(ride.record().jobMs == std::optional<double>(35.0));
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

TEST_CASE("app/elevator_ride: faded out, the ride waits for its floor; an idle wake moves no fade",
          "[app][elevator_ride]") {
    ElevatorRide ride(request(), RideSettings{}, 0);
    // An idle wake (no time) leaves the fade where it is.
    CHECK(ride.update(at(0, 1.0, false, 0.0)) == RideAction::None);
    CHECK(ride.fade() == 0.0f);
    for (std::uint64_t frame = 0; frame < 15; ++frame) {
        CHECK(ride.update(at(frame, 10.0)) == RideAction::None);
    }
    CHECK(ride.phase() == RidePhase::AwaitingFloor);
    CHECK(ride.awaitsFloor());
    CHECK(ride.update(at(15, 20.0, false, 0.0)) == RideAction::None);
    // The floor's job answers in a lockstep wait: the swap at that wake.
    ride.floorAnswered(true, 900.0, 880.0);
    CHECK_FALSE(ride.awaitsFloor());
    CHECK(ride.update(at(15, 905.0, false, 0.0)) == RideAction::Swap);
    ride.swapped(15);
    // A second answer (a stale one) changes nothing.
    ride.floorAnswered(false, 950.0, std::nullopt);
    CHECK_FALSE(ride.failed());
    CHECK(ride.record().builtMs == std::optional<double>(900.0));
}

TEST_CASE("app/elevator_ride: a destination that cannot be built leaves the walker where it was", "[app][elevator_ride]") {
    SECTION("while the view fades out: it comes back from where its fade stands") {
        ElevatorRide ride(request(), RideSettings{}, 10);
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
        ElevatorRide ride(request(), RideSettings{}, 10);
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

TEST_CASE("app/elevator_ride: the view comes back after its timeout outside lockstep, never under it",
          "[app][elevator_ride]") {
    for (const bool lockstep : {false, true}) {
        CAPTURE(lockstep);
        RideSettings settings;
        settings.lockstep = lockstep;
        ElevatorRide ride(request(), settings, 0);
        ride.floorAnswered(true, 1.0, 1.0);
        std::uint64_t frame = 0;
        while (ride.update(at(frame, 100.0)) != RideAction::Swap) {
            ++frame;
        }
        ride.swapped(frame);
        // 9.9 s after the swap: still black.
        CHECK(ride.update(at(++frame, 100.0 + 9900.0)) == RideAction::None);
        CHECK(ride.phase() == RidePhase::AwaitingView);
        // 10 s: the timeout (outside lockstep), the record saying so.
        CHECK(ride.update(at(++frame, 100.0 + 10000.0)) == RideAction::None);
        CHECK(ride.phase() == (lockstep ? RidePhase::AwaitingView : RidePhase::FadingIn));
        if (!lockstep) {
            RideAction action = RideAction::None;
            while (action != RideAction::Finish) {
                action = ride.update(at(++frame, 10500.0));
            }
            CHECK(ride.record().outcome == "timed-out");
            CHECK_FALSE(ride.record().residentMs.has_value());
        }
    }
}

TEST_CASE("app/elevator_ride: reduceMotion makes both fades instant", "[app][elevator_ride]") {
    RideSettings settings;
    settings.reduceMotion = true;
    ElevatorRide ride(request(RideReason::ShowInRoom), settings, 7);
    CHECK(ride.record().reason == "showInRoom");
    CHECK(ride.record().game == "g0000000000000001");
    ride.floorAnswered(true, 2.0, 2.0);
    CHECK(ride.update(at(7, 3.0)) == RideAction::Swap);
    CHECK(ride.record().fadedOutFrame == std::optional<std::uint64_t>(7));
    ride.swapped(7);
    CHECK(ride.update(at(8, 10.0, true)) == RideAction::None);
    CHECK(ride.update(at(9, 20.0)) == RideAction::Finish);
    CHECK(ride.record().fadedInFrame == std::optional<std::uint64_t>(9));
    CHECK(rv::app::toString(RideReason::Floor) == "floor");
    CHECK(rv::app::toString(RideReason::Room) == "room");
    CHECK(rv::app::toString(RidePhase::AwaitingView) == "awaiting-view");
}
