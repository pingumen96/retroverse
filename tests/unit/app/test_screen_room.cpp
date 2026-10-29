// Which room's CRT is the hero CRT on a floor (I4 step 6b; ADR-0013 §9): the room that holds the eye after the dwell,
// the doorway's hysteresis, the hall keeping the last room's; the bound screens found again by object id (I5 step 3's
// review).

#include <rv/app/screen_policy.hpp>
#include <rv/app/screen_room.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using rv::app::ScreenRoomTracker;

namespace {

/// The run's step under --fixed-dt 0.016667.
constexpr double kFrame = 0.016667;

/// `frames` frames with the eye in `room` (none: a hall) from `now`; the rooms bound meanwhile, the time after.
struct Walked {
    std::optional<std::string> bound;
    int binds = 0;
    double now = 0.0;
};

Walked walk(ScreenRoomTracker& tracker, std::optional<std::string_view> room, int frames, double now) {
    Walked walked;
    for (int frame = 0; frame < frames; ++frame) {
        if (auto bound = tracker.update(room, now)) {
            walked.bound = std::move(bound);
            ++walked.binds;
        }
        now += kFrame;
    }
    walked.now = now;
    return walked;
}

} // namespace

TEST_CASE("app/screen_room: the CRT of the room that holds the eye, bound after the dwell", "[app][screen]") {
    // The lobby start: nothing bound until the eye stays in a room for 0.3 s (18 frames of 1/60 s).
    ScreenRoomTracker tracker;
    CHECK_FALSE(tracker.bound().has_value());
    Walked hall = walk(tracker, std::nullopt, 30, 0.0);
    CHECK(hall.binds == 0);
    Walked early = walk(tracker, "area:arcade", 18, hall.now);
    CHECK(early.binds == 0);
    CHECK(tracker.candidate() == std::optional<std::string>("area:arcade"));
    Walked bound = walk(tracker, "area:arcade", 1, early.now);
    CHECK(bound.binds == 1);
    CHECK(bound.bound == std::optional<std::string>("area:arcade"));
    CHECK(tracker.bound() == std::optional<std::string>("area:arcade"));
    CHECK_FALSE(tracker.candidate().has_value());
    // Staying binds nothing more; the hall keeps the room's CRT.
    CHECK(walk(tracker, "area:arcade", 60, bound.now).binds == 0);
    const Walked out = walk(tracker, std::nullopt, 120, bound.now + 1.0);
    CHECK(out.binds == 0);
    CHECK(tracker.bound() == std::optional<std::string>("area:arcade"));
}

TEST_CASE("app/screen_room: a start room bound from the start, another room after its dwell, a doorway's flicker "
          "rebinding nothing",
          "[app][screen]") {
    ScreenRoomTracker tracker(std::string("room:snes"));
    CHECK(tracker.bound() == std::optional<std::string>("room:snes"));
    CHECK(walk(tracker, "room:snes", 60, 0.0).binds == 0);
    // At room B's doorway the eye crosses its edge every few frames: the dwell starts again each time.
    double now = 1.0;
    for (int crossing = 0; crossing < 20; ++crossing) {
        const Walked in = walk(tracker, "room:megadrive", 10, now);
        CHECK(in.binds == 0);
        now = walk(tracker, std::nullopt, 3, in.now).now;
    }
    CHECK(tracker.bound() == std::optional<std::string>("room:snes"));
    // Back and forth between two rooms faster than the dwell: nothing either.
    for (int crossing = 0; crossing < 10; ++crossing) {
        CHECK(walk(tracker, "room:megadrive", 12, now).binds == 0);
        now += 12 * kFrame;
        CHECK(walk(tracker, "room:snes", 12, now).binds == 0);
        now += 12 * kFrame;
    }
    // Then into room B for good.
    const Walked b = walk(tracker, "room:megadrive", 30, now);
    CHECK(b.binds == 1);
    CHECK(tracker.bound() == std::optional<std::string>("room:megadrive"));
    // A floor committed again: its bound room set from outside, any dwell forgotten.
    static_cast<void>(walk(tracker, "room:snes", 5, b.now));
    CHECK(tracker.candidate().has_value());
    tracker.reset(std::string("room:snes"));
    CHECK(tracker.bound() == std::optional<std::string>("room:snes"));
    CHECK_FALSE(tracker.candidate().has_value());
    tracker.reset(std::nullopt);
    CHECK_FALSE(tracker.bound().has_value());
    // An empty area is a hall.
    CHECK(walk(tracker, "", 60, b.now + 1.0).binds == 0);
    CHECK(rv::app::kScreenDwellSeconds == 0.3);
}

TEST_CASE("app/screen_room: the bound screens found again by object id, whatever their order", "[app][screen]") {
    // I5 step 3's review (ScreenService::rebind()): a grown room's hero and two secondary TVs, found again after the
    // room editor rebuilt the room's static part, the two TVs in the other order.
    using rv::app::rebindOrder;
    const std::vector<std::string> bound{"room:snes/fx:hero.crt", "room:snes/fx:w03", "room:snes/fx:e11"};
    CHECK(rebindOrder(bound, bound) == std::optional<std::vector<std::size_t>>({0, 1, 2}));
    const std::vector<std::string> reordered{"room:snes/fx:hero.crt", "room:snes/fx:e11", "room:snes/fx:w03"};
    CHECK(rebindOrder(bound, reordered) == std::optional<std::vector<std::size_t>>({0, 2, 1}));
    const std::vector<std::string> heroLast{"room:snes/fx:e11", "room:snes/fx:w03", "room:snes/fx:hero.crt"};
    CHECK(rebindOrder(bound, heroLast) == std::optional<std::vector<std::size_t>>({2, 1, 0}));
    // Not the bound screens: one missing, one more, another id, an id twice.
    CHECK_FALSE(rebindOrder(bound, std::vector<std::string>{"room:snes/fx:hero.crt", "room:snes/fx:w03"}));
    CHECK_FALSE(rebindOrder(bound, std::vector<std::string>{"room:snes/fx:hero.crt", "room:snes/fx:w03",
                                                            "room:snes/fx:e11", "room:snes/fx:e12"}));
    CHECK_FALSE(rebindOrder(bound, std::vector<std::string>{"room:snes/fx:hero.crt", "room:snes/fx:w03",
                                                            "room:snes/fx:e12"}));
    CHECK_FALSE(rebindOrder(bound, std::vector<std::string>{"room:snes/fx:hero.crt", "room:snes/fx:w03",
                                                            "room:snes/fx:w03"}));
    CHECK_FALSE(rebindOrder(std::vector<std::string>{}, std::vector<std::string>{"a"}));
    CHECK(rebindOrder(std::vector<std::string>{}, std::vector<std::string>{}) ==
          std::optional<std::vector<std::size_t>>(std::vector<std::size_t>{}));
}
