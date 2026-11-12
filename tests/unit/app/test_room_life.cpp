// The rooms' life (rv/app/room_life.hpp, I12 step 8, ADR-0021 §6): the clocks' time read from --clock and carried on
// with the run, and the angles the moving parts stand at, still under reduceMotion but for the clocks' hours and
// minutes.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>

#include <rv/app/room_life.hpp>

using Catch::Approx;
using rv::scene::ObjectMotion;

TEST_CASE("app/room_life: a clock's time, HH:MM or HH:MM:SS, carried on with the run", "[app][life]") {
    CHECK(rv::app::parseClockTime("10:10") == std::optional<double>(36600.0));
    CHECK(rv::app::parseClockTime("00:00:00") == std::optional<double>(0.0));
    CHECK(rv::app::parseClockTime("23:59:59") == std::optional<double>(86399.0));
    for (const char* bad : {"24:00", "12:60", "12:00:60", "9:30", "09:3", "09:30:", "0930", "09.30", "ab:cd", ""}) {
        CAPTURE(bad);
        CHECK_FALSE(rv::app::parseClockTime(bad).has_value());
    }
    CHECK(rv::app::kFrameLimitedClockSeconds == 36600.0);
    CHECK(rv::app::clockAfter(36600.0, 30.5) == Approx(36630.5));
    CHECK(rv::app::clockAfter(86390.0, 20.0) == Approx(10.0)); // past midnight
    CHECK(rv::app::clockAfter(100.0, std::nan("")) == Approx(100.0));
}

TEST_CASE("app/room_life: under reduceMotion the fans stand still and the second hands at twelve", "[app][life]") {
    ObjectMotion fan;
    fan.kind = ObjectMotion::Kind::Spin;
    fan.turnsPerSecond = 0.9f;
    ObjectMotion hour;
    hour.kind = ObjectMotion::Kind::ClockHour;
    ObjectMotion second;
    second.kind = ObjectMotion::Kind::ClockSecond;
    const double clock = 36600.0 + 15.0;
    CHECK(rv::app::lifeAngle(fan, 0.25, clock, false) != 0.0f);
    CHECK(rv::app::lifeAngle(fan, 0.25, clock, true) == 0.0f);
    CHECK(rv::app::lifeAngle(second, 0.25, clock, false) == Approx(-0.25f * 6.28318530718f));
    CHECK(rv::app::lifeAngle(second, 0.25, clock, true) == 0.0f);
    CHECK(rv::app::lifeAngle(hour, 0.25, clock, true) == rv::app::lifeAngle(hour, 0.25, clock, false));
    CHECK(rv::app::lifeAngle(hour, 0.25, clock, true) != 0.0f);
}
