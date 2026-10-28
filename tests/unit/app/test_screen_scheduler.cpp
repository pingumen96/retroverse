// The decoder scheduler (I5 step 2, ADR-0016 §2) without a GPU or a decoder: its tables frame by frame (sharing, the
// budget, the pause out of view and the stop after 10 s, a slot needed by a request in view, the hand-over rule, the
// ambient hysteresis, the late-frame drop) and, over long seeded runs, never more open decoders than the budget, never
// more threads than slots, and a slot reopened only once its previous thread ended.

#include <rv/app/screen_scheduler.hpp>
#include <rv/core/random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using rv::app::ScheduleResult;
using rv::app::ScreenAssignment;
using rv::app::ScreenClass;
using rv::app::ScreenRequest;
using rv::app::ScreenScheduler;
using rv::app::ScreenSchedulerSettings;
using rv::app::SlotAction;
using rv::app::SlotCommand;
using rv::app::SlotFeedback;
using rv::app::SlotState;

namespace {

/// The run's step under --fixed-dt 0.016667.
constexpr double kFrame = 0.016667;

ScreenRequest request(std::string id, ScreenClass screenClass, std::string clip, double distance, bool visible = true) {
    return ScreenRequest{std::move(id), screenClass, std::move(clip), distance, visible};
}

/// The four-CRT room of the GPU case (tests/smoke/scenes/screens_four.json): the hero and a secondary on clip a, two
/// secondaries on b (the nearer) and c.
std::vector<ScreenRequest> fourScreens() {
    return {request("crt_a", ScreenClass::Room, "a", 3.6), request("crt_b", ScreenClass::Ambient, "a", 3.1),
            request("crt_c", ScreenClass::Ambient, "b", 3.1), request("crt_d", ScreenClass::Ambient, "c", 4.0)};
}

/// The decoders as the service runs them: a stopped one's thread ends `drainFrames` updates later.
class Decoders {
public:
    explicit Decoders(std::size_t slots, int drainFrames = 3) : m_left(slots, -1), m_drainFrames(drainFrames) {}

    [[nodiscard]] std::vector<SlotFeedback> feedback() const {
        std::vector<SlotFeedback> feedback(m_left.size());
        for (std::size_t slot = 0; slot < m_left.size(); ++slot) {
            feedback[slot].threadRunning = m_left[slot] != -1;
        }
        return feedback;
    }

    /// Carries the frame's commands out, checking that a slot opens only once its thread ended.
    void apply(const ScheduleResult& result) {
        for (int& left : m_left) {
            if (left > 0) {
                --left;
            } else if (left == 0) {
                left = -1;
            }
        }
        for (const SlotCommand& command : result.commands) {
            int& left = m_left[command.slot];
            if (command.action == SlotAction::Open) {
                INFO("slot " << command.slot << " opened " << command.clip);
                CHECK(left == -1); // its previous thread ended
                left = -2;         // running
            } else if (command.action == SlotAction::Stop) {
                CHECK(left == -2);
                left = m_drainFrames;
            }
        }
    }

    [[nodiscard]] std::size_t threads() const {
        return static_cast<std::size_t>(std::count_if(m_left.begin(), m_left.end(), [](int left) { return left != -1; }));
    }

private:
    /// -1 no thread, -2 running, n >= 0: stopped, ending in n updates.
    std::vector<int> m_left;
    int m_drainFrames;
};

/// Runs `frames` updates of `requests` from `start`, carrying the commands out; the last result.
ScheduleResult run(ScreenScheduler& scheduler, Decoders& decoders, const std::vector<ScreenRequest>& requests,
                   double& now, int frames) {
    ScheduleResult result;
    for (int frame = 0; frame < frames; ++frame) {
        result = scheduler.update(now, requests, decoders.feedback());
        decoders.apply(result);
        now += kFrame;
    }
    return result;
}

std::vector<SlotCommand> commandsOf(const ScheduleResult& result, SlotAction action) {
    std::vector<SlotCommand> found;
    std::copy_if(result.commands.begin(), result.commands.end(), std::back_inserter(found),
                 [action](const SlotCommand& command) { return command.action == action; });
    return found;
}

ScreenAssignment playing(std::size_t slot) {
    return ScreenAssignment{slot, true, false};
}

} // namespace

TEST_CASE("app/screen_scheduler: 3 clips on 4 screens share 2 slots, the nearer ambient clip first",
          "[app][screen_scheduler]") {
    ScreenScheduler scheduler;
    Decoders decoders(2);
    const ScheduleResult first = scheduler.update(0.0, fourScreens(), decoders.feedback());
    CHECK(first.commands == std::vector<SlotCommand>{{SlotAction::Open, 0, "a", "wanted"},
                                                     {SlotAction::Open, 1, "b", "wanted"}});
    CHECK(first.screens == std::vector<ScreenAssignment>{playing(0), playing(0), playing(1), ScreenAssignment{}});
    decoders.apply(first);
    REQUIRE(scheduler.slots().size() == 2);
    CHECK(scheduler.slots()[0].screens == std::vector<std::string>{"crt_a", "crt_b"});
    CHECK(scheduler.slots()[1].screens == std::vector<std::string>{"crt_c"});
    // Nothing changes while nothing does.
    double now = kFrame;
    const ScheduleResult later = run(scheduler, decoders, fourScreens(), now, 120);
    CHECK(later.commands.empty());
    CHECK(later.screens == first.screens);
    CHECK(scheduler.openSlots() == 2);
    CHECK(scheduler.handOvers() == 0);
}

TEST_CASE("app/screen_scheduler: one decoder plays the best clip only", "[app][screen_scheduler]") {
    ScreenScheduler scheduler(ScreenSchedulerSettings{.maxDecoders = 1});
    Decoders decoders(1);
    const ScheduleResult first = scheduler.update(0.0, fourScreens(), decoders.feedback());
    CHECK(first.commands == std::vector<SlotCommand>{{SlotAction::Open, 0, "a", "wanted"}});
    CHECK(first.screens ==
          std::vector<ScreenAssignment>{playing(0), playing(0), ScreenAssignment{}, ScreenAssignment{}});
    // Out of range budgets are clamped: never more than 2 slots, never none.
    CHECK(ScreenScheduler(ScreenSchedulerSettings{.maxDecoders = 5}).slots().size() == 2);
    CHECK(ScreenScheduler(ScreenSchedulerSettings{.maxDecoders = 0}).slots().size() == 1);
}

TEST_CASE("app/screen_scheduler: a slot out of view pauses at once and stops after 10 s", "[app][screen_scheduler]") {
    ScreenScheduler scheduler;
    Decoders decoders(2);
    double now = 0.0;
    std::vector<ScreenRequest> requests = fourScreens();
    run(scheduler, decoders, requests, now, 60);
    // Turned away: c and d leave the view, a and b stay.
    requests[2].visible = false;
    requests[3].visible = false;
    const double turned = now;
    ScheduleResult result = scheduler.update(now, requests, decoders.feedback());
    decoders.apply(result);
    now += kFrame;
    CHECK(result.commands == std::vector<SlotCommand>{{SlotAction::Pause, 1, "b", "out-of-view"}});
    CHECK(result.screens[2] == playing(1)); // its frame kept
    CHECK(scheduler.slots()[1].paused);
    // d, out of view too, never takes a slot; the paused one is kept until 10 s have passed.
    int frames = 0;
    std::optional<double> stoppedAt;
    while (!stoppedAt && frames < 1000) {
        result = scheduler.update(now, requests, decoders.feedback());
        decoders.apply(result);
        for (const SlotCommand& command : result.commands) {
            CHECK(command.action == SlotAction::Stop);
            CHECK(command.slot == 1u);
            CHECK(command.reason == "out-of-view");
            stoppedAt = now;
        }
        now += kFrame;
        ++frames;
    }
    REQUIRE(stoppedAt.has_value());
    CHECK(*stoppedAt - turned >= 10.0 - 1e-6);
    CHECK(*stoppedAt - turned < 10.0 + kFrame);
    CHECK(result.screens[2] == ScreenAssignment{}); // its still now
    // Nothing reopens while c and d stay out of view.
    result = run(scheduler, decoders, requests, now, 60);
    CHECK(result.commands.empty());
    CHECK(scheduler.slots()[1].state == SlotState::Free);
    // Back in view: c opens again at once (a free slot changes no hands).
    requests[2].visible = true;
    requests[3].visible = true;
    result = scheduler.update(now, requests, decoders.feedback());
    CHECK(result.commands == std::vector<SlotCommand>{{SlotAction::Open, 1, "b", "wanted"}});
}

TEST_CASE("app/screen_scheduler: a request in view takes a paused slot, which reopens only once its thread ended",
          "[app][screen_scheduler]") {
    ScreenScheduler scheduler;
    Decoders decoders(2, 5);
    double now = 0.0;
    std::vector<ScreenRequest> requests = fourScreens();
    run(scheduler, decoders, requests, now, 90);
    // c leaves the view; d, still in view, needs its slot (held for more than a second).
    requests[2].visible = false;
    ScheduleResult result = scheduler.update(now, requests, decoders.feedback());
    decoders.apply(result);
    now += kFrame;
    CHECK(result.commands == std::vector<SlotCommand>{{SlotAction::Stop, 1, "b", "needed"}});
    CHECK(result.screens[3] == (ScreenAssignment{1, false, true})); // reserved, waiting for the thread
    CHECK(result.screens[2] == ScreenAssignment{});
    CHECK(scheduler.handOvers() == 1);
    // The thread runs for 5 more updates: no Open before it ended (Decoders checks it).
    int waited = 0;
    while (commandsOf(result, SlotAction::Open).empty() && waited < 20) {
        result = scheduler.update(now, requests, decoders.feedback());
        decoders.apply(result);
        now += kFrame;
        ++waited;
    }
    CHECK(waited == 7);
    CHECK(result.commands == std::vector<SlotCommand>{{SlotAction::Open, 1, "c", "wanted"}});
    CHECK(result.screens[3] == playing(1));
}

TEST_CASE("app/screen_scheduler: a slot changes hands at most once a second unless focus asks",
          "[app][screen_scheduler]") {
    ScreenScheduler scheduler(ScreenSchedulerSettings{.maxDecoders = 1});
    Decoders decoders(1, 0);
    double now = 0.0;
    // Two ambient screens whose distances swap every frame: the nearer one would take the slot each time.
    std::vector<std::size_t> opensPerSecond(4, 0);
    for (int frame = 0; frame < 240; ++frame) {
        const bool even = frame % 2 == 0;
        const std::vector<ScreenRequest> requests{request("tv_1", ScreenClass::Ambient, "x", even ? 2.0 : 3.0),
                                                  request("tv_2", ScreenClass::Ambient, "y", even ? 3.0 : 2.0)};
        const ScheduleResult result = scheduler.update(now, requests, decoders.feedback());
        decoders.apply(result);
        opensPerSecond[static_cast<std::size_t>(frame / 60)] += commandsOf(result, SlotAction::Open).size();
        now += kFrame;
    }
    for (std::size_t second = 0; second < opensPerSecond.size(); ++second) {
        INFO("second " << second);
        CHECK(opensPerSecond[second] <= 2); // one hand-over a second, its open the next frame at the latest
    }
    // Settled on x; then a focus request takes the slot at once, whoever had it a moment ago.
    run(scheduler, decoders, {request("tv_1", ScreenClass::Ambient, "x", 2.0)}, now, 150);
    REQUIRE(scheduler.slots()[0].state == SlotState::Open);
    REQUIRE(scheduler.slots()[0].clip == "x");
    const std::vector<ScreenRequest> focused{request("tv_1", ScreenClass::Ambient, "x", 2.0),
                                             request("hero", ScreenClass::Focus, "z", 5.0)};
    const std::uint64_t before = scheduler.handOvers();
    ScheduleResult result = scheduler.update(now, focused, decoders.feedback());
    decoders.apply(result);
    CHECK(scheduler.handOvers() == before + 1);
    CHECK(commandsOf(result, SlotAction::Stop).size() == 1);
    now += kFrame;
    for (int frame = 0; frame < 5 && commandsOf(result, SlotAction::Open).empty(); ++frame) {
        result = scheduler.update(now, focused, decoders.feedback());
        decoders.apply(result);
        now += kFrame;
    }
    CHECK(result.commands == std::vector<SlotCommand>{{SlotAction::Open, 0, "z", "wanted"}});
}

TEST_CASE("app/screen_scheduler: a clip nobody asks for stops at once, a switch reopens the slot", "[app][screen_scheduler]") {
    ScreenScheduler scheduler(ScreenSchedulerSettings{.maxDecoders = 1});
    Decoders decoders(1, 2);
    double now = 0.0;
    run(scheduler, decoders, {request("hero", ScreenClass::Room, "a", 3.0)}, now, 3);
    // The playlist's next game: its clip waits for the slot's thread, the old one stopped at once (no hand-over).
    const std::vector<ScreenRequest> next{request("hero", ScreenClass::Room, "b", 3.0)};
    ScheduleResult result = scheduler.update(now, next, decoders.feedback());
    decoders.apply(result);
    now += kFrame;
    CHECK(result.commands == std::vector<SlotCommand>{{SlotAction::Stop, 0, "a", "unwanted"}});
    CHECK(result.screens.front() == (ScreenAssignment{0, false, true}));
    CHECK(scheduler.handOvers() == 0);
    result = run(scheduler, decoders, next, now, 6);
    CHECK(result.screens.front() == playing(0));
    // No request at all (a launch, autoplay off): stopped.
    result = scheduler.update(now, std::vector{request("hero", ScreenClass::Room, "", 3.0)}, decoders.feedback());
    CHECK(result.commands == std::vector<SlotCommand>{{SlotAction::Stop, 0, "b", "unwanted"}});
}

TEST_CASE("app/screen_scheduler: ambient requests enter within proximityMeters and leave 0.5 m beyond it",
          "[app][screen_scheduler]") {
    ScreenScheduler scheduler(ScreenSchedulerSettings{.maxDecoders = 2, .proximityMeters = 12.0});
    Decoders decoders(2, 0);
    double now = 0.0;
    // distance -> counted: entering only within 12 m, then kept up to 12.5 m.
    const std::vector<std::pair<double, bool>> walk{{12.2, false}, {12.0, true},  {12.4, true},
                                                    {12.5, true},  {12.6, false}, {12.3, false},
                                                    {11.9, true},  {12.49, true}, {13.0, false}};
    for (const auto& [distance, counted] : walk) {
        INFO("at " << distance << " m");
        const ScheduleResult result =
            scheduler.update(now, std::vector{request("tv", ScreenClass::Ambient, "x", distance)}, decoders.feedback());
        decoders.apply(result);
        CHECK(result.screens.front().slot.has_value() == counted);
        now += 1.5; // past the hand-over rule
    }
    // The other classes have no proximity.
    const ScheduleResult hero =
        scheduler.update(now, std::vector{request("hero", ScreenClass::Room, "x", 40.0)}, decoders.feedback());
    CHECK(hero.screens.front().slot.has_value());
}

TEST_CASE("app/screen_scheduler: late frames drop the budget to one for 30 s, the lowest class losing its slot",
          "[app][screen_scheduler]") {
    ScreenScheduler scheduler;
    Decoders decoders(2, 0);
    const std::vector<ScreenRequest> requests{request("hero", ScreenClass::Room, "a", 3.0),
                                              request("tv", ScreenClass::Ambient, "b", 2.0)};
    double now = 0.0;
    // Slot 0 presents 30 frames a second, 1 in 10 late from frame 60 on; slot 1 is clean.
    std::uint64_t presented = 0;
    std::uint64_t late = 0;
    std::optional<double> droppedAt;
    for (int frame = 0; frame < 600 && !droppedAt; ++frame) {
        std::vector<SlotFeedback> feedback = decoders.feedback();
        if (frame % 2 == 0) {
            ++presented;
            if (frame >= 60 && frame % 20 == 0) {
                ++late;
            }
        }
        feedback[0].presented = presented;
        feedback[0].late = late;
        feedback[1].presented = presented;
        const ScheduleResult result = scheduler.update(now, requests, feedback);
        decoders.apply(result);
        for (const SlotCommand& command : commandsOf(result, SlotAction::Stop)) {
            CHECK(command == SlotCommand{SlotAction::Stop, 1, "b", "budget"});
            droppedAt = now;
        }
        now += kFrame;
    }
    REQUIRE(droppedAt.has_value());
    // The window needs 4 s of play; the ratio passes 5% once it holds enough late frames.
    CHECK(*droppedAt >= 4.0);
    CHECK(*droppedAt < 8.0);
    REQUIRE(scheduler.drops().size() == 1);
    CHECK(scheduler.drops().front().slot == 0u);
    CHECK(scheduler.drops().front().ratio > 0.05);
    CHECK(scheduler.budget(now) == 1);
    // 30 s later the budget is 2 again and the ambient clip comes back (clean frames meanwhile).
    ScheduleResult result = run(scheduler, decoders, requests, now, static_cast<int>(29.0 / kFrame));
    CHECK(scheduler.budget(now) == 1);
    CHECK(result.screens[1] == ScreenAssignment{});
    result = run(scheduler, decoders, requests, now, static_cast<int>(1.5 / kFrame));
    CHECK(scheduler.budget(now) == 2);
    CHECK(result.screens[1] == playing(1));
    CHECK(scheduler.drops().size() == 1);
}

TEST_CASE("app/screen_scheduler: stopAll stops every open slot and drops the reservations", "[app][screen_scheduler]") {
    ScreenScheduler scheduler;
    Decoders decoders(2);
    double now = 0.0;
    run(scheduler, decoders, fourScreens(), now, 5);
    const std::vector<SlotCommand> stops = scheduler.stopAll("launch");
    CHECK(stops == std::vector<SlotCommand>{{SlotAction::Stop, 0, "a", "launch"}, {SlotAction::Stop, 1, "b", "launch"}});
    CHECK(scheduler.openSlots() == 0);
    CHECK(scheduler.stopAll("launch").empty());
    // An open that failed frees its slot at once.
    ScreenScheduler other(ScreenSchedulerSettings{.maxDecoders = 1});
    const ScheduleResult opened = other.update(0.0, fourScreens(), {});
    REQUIRE(opened.commands.size() == 1);
    other.openFailed(0);
    CHECK(other.slots()[0].state == SlotState::Free);
}

TEST_CASE("app/screen_scheduler: never more decoders than the budget nor threads than slots, over seeded runs",
          "[app][screen_scheduler]") {
    for (const int budget : {1, 2}) {
        for (std::uint64_t seed = 1; seed <= 20; ++seed) {
            INFO("budget " << budget << ", seed " << seed);
            rv::core::Random random(seed * 7919u + static_cast<std::uint64_t>(budget));
            ScreenScheduler scheduler(ScreenSchedulerSettings{.maxDecoders = budget, .proximityMeters = 6.0});
            Decoders decoders(static_cast<std::size_t>(budget), static_cast<int>(random.below(8)));
            std::vector<ScreenRequest> requests{request("hero", ScreenClass::Room, "a", 3.0),
                                                request("tv_1", ScreenClass::Ambient, "b", 4.0),
                                                request("tv_2", ScreenClass::Ambient, "c", 5.0),
                                                request("tv_3", ScreenClass::Ambient, "a", 7.0)};
            double now = 0.0;
            std::size_t maxThreads = 0;
            for (int frame = 0; frame < 3000; ++frame) {
                // Now and then a screen turns, moves, switches clip or is focused.
                for (ScreenRequest& r : requests) {
                    switch (random.below(40)) {
                    case 0:
                        r.visible = !r.visible;
                        break;
                    case 1:
                        r.distanceMeters = 1.0 + static_cast<double>(random.below(80)) / 10.0;
                        break;
                    case 2:
                        r.clip = std::string(1, static_cast<char>('a' + random.below(5)));
                        break;
                    case 3:
                        r.clip.clear();
                        break;
                    case 4:
                        if (r.screenId == "hero") {
                            r.screenClass = r.screenClass == ScreenClass::Room ? ScreenClass::Focus : ScreenClass::Room;
                        }
                        break;
                    default:
                        break;
                    }
                }
                const ScheduleResult result = scheduler.update(now, requests, decoders.feedback());
                decoders.apply(result); // checks every Open came after its slot's thread ended
                REQUIRE(scheduler.openSlots() <= static_cast<std::size_t>(scheduler.budget(now)));
                maxThreads = std::max(maxThreads, decoders.threads());
                REQUIRE(decoders.threads() <= static_cast<std::size_t>(budget));
                // Each screen's assignment names a slot holding its clip.
                for (std::size_t index = 0; index < requests.size(); ++index) {
                    const ScreenAssignment& assignment = result.screens[index];
                    if (assignment.slot) {
                        const auto& slot = scheduler.slots()[*assignment.slot];
                        CHECK((assignment.playing ? slot.clip : slot.next) == requests[index].clip);
                    }
                }
                now += kFrame;
            }
            CHECK(maxThreads <= static_cast<std::size_t>(budget));
        }
    }
}
