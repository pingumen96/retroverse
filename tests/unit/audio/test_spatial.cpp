// ADR-0016 §9's spatial rules (I5 step 5): the gain table, the equal-power pan, the sync's decisions.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

#include <rv/audio/audio_sync.hpp>
#include <rv/audio/spatial.hpp>

using Catch::Approx;
using namespace rv::audio;

TEST_CASE("audio/spatial: the gain table: 1 within 1.5 m, then 1.5/d, fading over the last 2 m", "[audio][spatial]") {
    struct Row {
        double distance;
        double proximity;
        double gain;
    };
    const std::vector<Row> table = {
        {0.0, 12.0, 1.0},
        {1.0, 12.0, 1.0},
        {1.5, 12.0, 1.0},
        {3.0, 12.0, 0.5},
        {6.0, 12.0, 0.25},
        {9.0, 12.0, 1.0 / 6.0},
        {10.0, 12.0, 0.15},              // the fade starts 2 m before the proximity
        {11.0, 12.0, 1.5 / 11.0 * 0.5},  // half way through it
        {11.9, 12.0, 1.5 / 11.9 * 0.05}, // nearly silent
        {12.0, 12.0, 0.0},
        {15.0, 12.0, 0.0},
        {1.0, 3.0, 1.0},
        {2.0, 3.0, 0.75 * 0.5}, // a small proximity: the fade covers more of it
        {0.5, 1.0, 0.25},       // shorter than the fade: it fades from the eye on
        {0.0, 0.0, 0.0},        // no proximity: never heard
    };
    for (const Row& row : table) {
        INFO("distance " << row.distance << " m, proximity " << row.proximity << " m");
        CHECK(spatialGain(row.distance, row.proximity) == Approx(row.gain).margin(1e-12));
    }
    CHECK(spatialGain(std::nan(""), 12.0) == 0.0);
    // Never rising with the distance.
    double previous = 1.0;
    for (double distance = 0.0; distance <= 13.0; distance += 0.05) {
        const double gain = spatialGain(distance, 12.0);
        CHECK(gain <= previous + 1e-12);
        previous = gain;
    }
}

TEST_CASE("audio/spatial: the pan from the azimuth, equal power, at most 0.6 either side", "[audio][spatial]") {
    constexpr double kDegree = std::numbers::pi / 180.0;
    struct Row {
        double azimuthDegrees;
        double pan;
    };
    const std::vector<Row> table = {
        {0.0, 0.0},   {20.0, std::sin(20.0 * kDegree)}, {-20.0, -std::sin(20.0 * kDegree)},
        {36.87, 0.6}, {90.0, 0.6},                      {-90.0, -0.6},
        {150.0, 0.5}, {180.0, 0.0},                     {-150.0, -0.5},
    };
    for (const Row& row : table) {
        INFO("azimuth " << row.azimuthDegrees);
        CHECK(panForAzimuth(row.azimuthDegrees * kDegree) == Approx(row.pan).margin(1e-4));
    }
    const StereoGains centre = equalPowerGains(0.0);
    CHECK(centre.left == 1.0); // exactly: a centred source passes unchanged
    CHECK(centre.right == 1.0);
    for (double pan = -1.0; pan <= 1.0; pan += 0.1) {
        const StereoGains gains = equalPowerGains(pan);
        INFO("pan " << pan);
        CHECK(gains.left * gains.left + gains.right * gains.right == Approx(2.0));
        CHECK(gains.left >= 0.0);
        CHECK(gains.right >= 0.0);
    }
    const StereoGains right = equalPowerGains(0.6);
    CHECK(right.right > right.left);
    CHECK(right.left == Approx(std::cos(0.4 * std::numbers::pi) * std::numbers::sqrt2));
    CHECK(equalPowerGains(1.0).left == Approx(0.0).margin(1e-12));
    CHECK(decibelsFromGain(gainFromDecibels(kDuckDecibels)) == Approx(-9.0));
    CHECK(gainFromDecibels(kDuckDecibels) == Approx(0.35481).margin(1e-5));
}

TEST_CASE("audio/sync: nothing within 20 ms, a correction up to 1% within 120 ms, a resync beyond",
          "[audio][sync]") {
    struct Row {
        double error;
        SyncAction action;
        double ratio;
    };
    // From a source in sync each time (a fresh AudioSync).
    const std::vector<Row> table = {
        {0.0, SyncAction::None, 1.0},
        {0.019, SyncAction::None, 1.0},
        {-0.019, SyncAction::None, 1.0},
        {-0.025, SyncAction::Correct, 1.0 + 0.025 / 0.03 * 0.01}, // behind by 25 ms: most of the correction, faster
        {0.025, SyncAction::Correct, 1.0 - 0.025 / 0.03 * 0.01},  // ahead: slower
        {-0.03, SyncAction::Correct, 1.01},                       // the full 1% from 30 ms
        {-0.06, SyncAction::Correct, 1.01},
        {-0.119, SyncAction::Correct, 1.01},
        {0.1, SyncAction::Correct, 0.99},
        {0.121, SyncAction::Resync, 1.0},
        {-0.5, SyncAction::Resync, 1.0},
        {std::nan(""), SyncAction::Resync, 1.0},
    };
    for (const Row& row : table) {
        INFO("error " << row.error);
        AudioSync sync;
        const SyncDecision decision = sync.update(row.error);
        CHECK(decision.action == row.action);
        CHECK(decision.ratio == Approx(row.ratio));
    }
    // Once correcting, it goes on until the error is within 5 ms.
    AudioSync sync;
    CHECK(sync.update(-0.03).action == SyncAction::Correct);
    CHECK(sync.correcting());
    const SyncDecision going = sync.update(-0.01);
    CHECK(going.action == SyncAction::Correct);
    CHECK(going.ratio == Approx(1.0 + 0.01 / 0.03 * 0.01));
    CHECK(sync.update(-0.004).action == SyncAction::None);
    CHECK_FALSE(sync.correcting());
    CHECK(sync.update(-0.01).action == SyncAction::None); // within the deadband again
}
