#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <rv/app/frame_timing.hpp>

using rv::app::FrameTiming;
using rv::app::PhaseRecord;
using rv::app::TimeSummary;

TEST_CASE("app/frame_timing: the run's frame times leave the warm-up out", "[app][frame_timing]") {
    FrameTiming timing(3);
    CHECK(timing.runTimes() == rv::app::FrameTimesRecord{3, std::nullopt});
    // Frame 0 has no previous frame; frames 1 and 2 are the warm-up.
    timing.recordFrame(0, std::nullopt);
    timing.recordFrame(1, 500.0);
    timing.recordFrame(2, 400.0);
    CHECK(timing.runTimes() == rv::app::FrameTimesRecord{3, std::nullopt});
    for (std::uint64_t frame = 3; frame < 13; ++frame) {
        timing.recordFrame(frame, static_cast<double>(frame - 2)); // 1 to 10 ms
    }
    // Unknown and invalid times count for nothing.
    timing.recordFrame(13, std::nullopt);
    timing.recordFrame(14, -1.0);
    timing.recordFrame(15, std::numeric_limits<double>::infinity());
    const rv::app::FrameTimesRecord run = timing.runTimes();
    CHECK(run.warmUpFrames == 3);
    CHECK(run.times == TimeSummary{10, 5.0, 10.0, 10.0, 10.0});
    CHECK(timing.phases().empty()); // a run without phases reports none

    CHECK(FrameTiming().runTimes().warmUpFrames == rv::app::kFrameTimeWarmUpFrames);
    CHECK(rv::app::kFrameTimeWarmUpFrames == 30);
}

TEST_CASE("app/frame_timing: a phase counts the frames it is open at the end of", "[app][frame_timing]") {
    FrameTiming timing(0);
    timing.recordFrame(0, 1.0);
    // Opened during frame 1, closed during frame 4: frames 1, 2 and 3 are the phase's.
    timing.openPhase(rv::app::kPhaseBenchmark);
    CHECK(timing.phaseOpen("benchmark"));
    timing.recordFrame(1, 2.0);
    timing.recordFrame(2, 3.0);
    timing.recordFrame(3, 4.0);
    CHECK(timing.closePhase("benchmark"));
    CHECK_FALSE(timing.phaseOpen("benchmark"));
    CHECK_FALSE(timing.closePhase("benchmark")); // not open any more
    timing.recordFrame(4, 50.0);

    // Opened and closed within frame 5: that frame is the phase's.
    timing.openPhase(rv::app::kPhaseAnimation);
    CHECK(timing.closePhase(rv::app::kPhaseAnimation));
    timing.recordFrame(5, 6.0);
    timing.recordFrame(6, 7.0);

    const std::vector<PhaseRecord> phases = timing.phases();
    REQUIRE(phases.size() == 2);
    // In the order of their first opening.
    CHECK(phases[0] == PhaseRecord{"benchmark", 1, false, 3, TimeSummary{3, 3.0, 4.0, 4.0, 4.0}});
    CHECK(phases[1] == PhaseRecord{"animation", 1, false, 1, TimeSummary{1, 6.0, 6.0, 6.0, 6.0}});
    CHECK(timing.runTimes().times->count == 7);
}

TEST_CASE("app/frame_timing: phases nest by name and may stay open", "[app][frame_timing]") {
    FrameTiming timing(2);
    timing.openPhase(rv::app::kPhaseAnimation);
    timing.openPhase(rv::app::kPhaseAnimation); // a second animation at once
    timing.openPhase(rv::app::kPhaseRefresh);
    timing.openPhase(""); // ignored
    CHECK_FALSE(timing.closePhase(""));
    CHECK_FALSE(timing.closePhase(rv::app::kPhaseCoverLoad)); // never opened
    timing.recordFrame(0, std::nullopt);
    timing.recordFrame(1, 9.0);
    CHECK(timing.closePhase(rv::app::kPhaseAnimation));
    CHECK(timing.phaseOpen(rv::app::kPhaseAnimation)); // the first opening still holds it
    timing.recordFrame(2, 3.0);
    CHECK(timing.closePhase(rv::app::kPhaseAnimation));
    timing.recordFrame(3, 4.0);
    // Reopened later: the same record, its frames together.
    timing.openPhase(rv::app::kPhaseAnimation);
    timing.recordFrame(4, 5.0);
    CHECK(timing.closePhase(rv::app::kPhaseAnimation));

    const std::vector<PhaseRecord> phases = timing.phases();
    REQUIRE(phases.size() == 2);
    // Frames 0 to 2 and 4; frames 0 and 1 are the run's warm-up, so only two are timed.
    CHECK(phases[0] == PhaseRecord{"animation", 3, false, 4, TimeSummary{2, 3.0, 5.0, 5.0, 5.0}});
    // Still open when the run ended; within the warm-up its first frames have no time.
    CHECK(phases[1] == PhaseRecord{"refresh", 1, true, 5, TimeSummary{3, 4.0, 5.0, 5.0, 5.0}});

    // A phase that only covered warm-up frames reports its frames and no time.
    FrameTiming early(30);
    early.openPhase(rv::app::kPhaseCoverLoad);
    early.recordFrame(0, std::nullopt);
    early.recordFrame(1, 12.0);
    CHECK(early.closePhase(rv::app::kPhaseCoverLoad));
    CHECK(early.phases() == std::vector<PhaseRecord>{PhaseRecord{"coverLoad", 1, false, 2, std::nullopt}});
}
