#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include <rv/media/presentation_clock.hpp>

using Catch::Approx;
using rv::media::ClockSettings;
using rv::media::ClockUpdate;
using rv::media::FrameSource;
using rv::media::FrameTiming;
using rv::media::PresentationClock;

namespace {

/// Synthetic frames: a list of timings; records what the clock showed and skipped.
class FakeFrames final : public FrameSource {
public:
    /// Appends `count` frames of `fps` starting at `firstPts`.
    void add(int count, double fps, double firstPts = 0.0, std::uint32_t generation = 0) {
        for (int index = 0; index < count; ++index) {
            m_queue.push_back(FrameTiming{firstPts + index / fps, 1.0 / fps, generation});
        }
    }

    [[nodiscard]] std::optional<FrameTiming> peek(std::size_t index) const override {
        if (index >= m_queue.size()) {
            return std::nullopt;
        }
        return m_queue[index];
    }

    void pop(bool present) override {
        (present ? shown : skipped).push_back(m_queue.front().ptsSeconds);
        m_queue.pop_front();
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_queue.size(); }

    std::vector<double> shown;
    std::vector<double> skipped;

private:
    std::deque<FrameTiming> m_queue;
};

constexpr double kRefresh = 1.0 / 60.0;

} // namespace

TEST_CASE("media/presentation_clock: nothing is shown before the first frame", "[media][presentation_clock]") {
    PresentationClock clock;
    FakeFrames frames;
    const ClockUpdate update = clock.update(1.0, frames);
    CHECK_FALSE(update.newFrame);
    CHECK_FALSE(update.shown);
    CHECK_FALSE(update.mediaTime);
    CHECK_FALSE(clock.mediaTime(1.0));
    CHECK(clock.stats().updates == 1);
    CHECK(clock.stats().presented == 0);
}

TEST_CASE("media/presentation_clock: the media time starts at the first frame shown", "[media][presentation_clock]") {
    PresentationClock clock;
    FakeFrames frames;
    frames.add(10, 30.0, 12.0);
    const ClockUpdate first = clock.update(100.0, frames);
    CHECK(first.newFrame);
    REQUIRE(first.shown);
    CHECK(first.shown->ptsSeconds == 12.0);
    CHECK(clock.mediaTime(100.0) == Approx(12.0));
    CHECK(clock.mediaTime(100.25) == Approx(12.25));
    CHECK_FALSE(first.late);
    CHECK(frames.shown == std::vector<double>{12.0});
}

TEST_CASE("media/presentation_clock: a 30 fps video on a 60 Hz display shows every frame for two refreshes",
          "[media][presentation_clock]") {
    PresentationClock clock;
    FakeFrames frames;
    frames.add(300, 30.0);
    int newFrames = 0;
    for (int refresh = 0; refresh < 600; ++refresh) {
        // Render timestamps jitter by up to half a millisecond around the refresh.
        const double jitter = ((refresh * 7) % 5 - 2) * 0.00025;
        const ClockUpdate update = clock.update(refresh * kRefresh + jitter, frames);
        if (update.newFrame) {
            ++newFrames;
            CHECK(refresh % 2 == 0);
        }
    }
    CHECK(newFrames == 300);
    CHECK(clock.stats().presented == 300);
    CHECK(clock.stats().dropped == 0);
    CHECK(clock.stats().late == 0);
    CHECK(clock.stats().repeated == 0);
    CHECK(clock.stats().resyncs == 0);
    CHECK(frames.skipped.empty());
    CHECK(clock.stats().maxLatenessSeconds < 0.0015); // the render jitter only
}

TEST_CASE("media/presentation_clock: after a stall the newest due frame is shown and the older ones dropped",
          "[media][presentation_clock]") {
    PresentationClock clock;
    FakeFrames frames;
    frames.add(10, 30.0);
    CHECK(clock.update(0.0, frames).newFrame);
    const ClockUpdate afterStall = clock.update(0.2, frames); // frames up to 0.2 s are due
    CHECK(afterStall.newFrame);
    REQUIRE(afterStall.shown);
    CHECK(afterStall.shown->ptsSeconds == Approx(0.2));
    CHECK(afterStall.dropped == 5);
    CHECK_FALSE(afterStall.late); // it became due during the stall: shown at the first chance
    CHECK(clock.stats().dropped == 5);
    CHECK(frames.skipped.size() == 5);
    CHECK(frames.size() == 3);
}

TEST_CASE("media/presentation_clock: an empty queue repeats the frame on screen, and its successor is late",
          "[media][presentation_clock]") {
    PresentationClock clock;
    FakeFrames frames;
    frames.add(2, 30.0);
    int repeated = 0;
    for (int refresh = 0; refresh <= 6; ++refresh) {
        repeated += clock.update(refresh * kRefresh, frames).repeated ? 1 : 0;
    }
    // Frame 1 (1/30 s) is shown at 2/60 and has had its time from 4/60 on: refreshes 5 and 6 repeat it (4/60 is
    // within the due tolerance of its end).
    CHECK(repeated == 2);
    CHECK(clock.stats().repeated == 2);

    frames.add(1, 30.0, 2.0 / 30.0); // the decoder catches up: frame 2 arrives late
    const ClockUpdate update = clock.update(7 * kRefresh, frames);
    CHECK(update.newFrame);
    CHECK(update.late);
    CHECK_FALSE(update.resynced);
    CHECK(clock.stats().late == 1);
    CHECK(clock.stats().maxLatenessSeconds == Approx(7 * kRefresh - 2.0 / 30.0));
}

TEST_CASE("media/presentation_clock: an ended source does not count repeats", "[media][presentation_clock]") {
    PresentationClock clock;
    FakeFrames frames;
    frames.add(1, 30.0);
    for (int refresh = 0; refresh < 10; ++refresh) {
        CHECK_FALSE(clock.update(refresh * kRefresh, frames, true).repeated);
    }
    CHECK(clock.stats().repeated == 0);
    CHECK(clock.current());
}

TEST_CASE("media/presentation_clock: a frame far behind its time resyncs the clock", "[media][presentation_clock]") {
    PresentationClock clock;
    FakeFrames frames;
    frames.add(1, 30.0);
    CHECK(clock.update(0.0, frames).newFrame);
    frames.add(2, 30.0, 1.0 / 30.0);
    const ClockUpdate late = clock.update(1.0, frames); // nearly a second late
    CHECK(late.newFrame);
    CHECK(late.resynced);
    CHECK(late.dropped == 1);
    REQUIRE(late.shown);
    CHECK(late.shown->ptsSeconds == Approx(2.0 / 30.0));
    CHECK(clock.mediaTime(1.0) == Approx(2.0 / 30.0));
    CHECK(clock.stats().resyncs == 1);
    CHECK(clock.stats().resyncSeconds == Approx(1.0 - 2.0 / 30.0));

    frames.add(1, 30.0, 3.0 / 30.0);
    CHECK_FALSE(clock.update(1.0 + 0.5 / 30.0, frames).newFrame);
    const ClockUpdate onTime = clock.update(1.0 + 1.0 / 30.0, frames);
    CHECK(onTime.newFrame);
    CHECK_FALSE(onTime.late);
}

TEST_CASE("media/presentation_clock: pausing freezes the media time and takes no frame",
          "[media][presentation_clock]") {
    PresentationClock clock;
    FakeFrames frames;
    frames.add(30, 30.0);
    CHECK(clock.update(0.0, frames).newFrame);
    clock.pause(0.1);
    CHECK(clock.paused());
    CHECK(clock.mediaTime(5.0) == Approx(0.1));
    const ClockUpdate paused = clock.update(5.0, frames);
    CHECK_FALSE(paused.newFrame);
    CHECK(paused.mediaTime == Approx(0.1));
    CHECK(frames.shown.size() == 1);
    CHECK(clock.stats().repeated == 0);

    clock.resume(5.0);
    CHECK_FALSE(clock.paused());
    CHECK(clock.mediaTime(5.05) == Approx(0.15));
    const ClockUpdate resumed = clock.update(5.05, frames);
    CHECK(resumed.newFrame);
    REQUIRE(resumed.shown);
    CHECK(resumed.shown->ptsSeconds == Approx(4.0 / 30.0));
    CHECK_FALSE(resumed.late);
}

TEST_CASE("media/presentation_clock: a newer generation restarts the clock, older frames are thrown away",
          "[media][presentation_clock]") {
    PresentationClock clock;
    FakeFrames frames;
    frames.add(3, 30.0, 0.0, 0);
    CHECK(clock.update(0.0, frames).newFrame);
    CHECK(clock.generation() == 0);

    // A seek: the decoder discards what was queued and sends the frames after the target with the next generation.
    FakeFrames afterSeek;
    afterSeek.add(1, 30.0, 5.0, 1);
    const ClockUpdate update = clock.update(0.5, afterSeek);
    CHECK(clock.generation() == 1);
    CHECK(update.newFrame);
    REQUIRE(update.shown);
    CHECK(update.shown->ptsSeconds == Approx(5.0));
    CHECK(update.shown->generation == 1);
    CHECK(clock.mediaTime(0.5) == Approx(5.0));
    CHECK_FALSE(update.late);

    // Frames of an older generation are thrown away.
    FakeFrames mixed;
    mixed.add(2, 30.0, 0.1, 0);
    mixed.add(1, 30.0, 5.0 + 1.0 / 30.0, 1);
    const ClockUpdate next = clock.update(0.55, mixed);
    CHECK(clock.stats().stale == 2);
    CHECK(mixed.skipped.size() == 2);
    CHECK(next.newFrame);
    REQUIRE(next.shown);
    CHECK(next.shown->ptsSeconds == Approx(5.0 + 1.0 / 30.0));
}

TEST_CASE("media/presentation_clock: a frame within the due tolerance is shown", "[media][presentation_clock]") {
    ClockSettings settings;
    settings.dueToleranceSeconds = 0.002;
    PresentationClock clock(settings);
    FakeFrames frames;
    frames.add(2, 10.0);
    CHECK(clock.update(0.0, frames).newFrame);
    CHECK(clock.update(0.0985, frames).newFrame); // 1.5 ms early
    CHECK(clock.stats().presented == 2);
    CHECK(clock.settings().dueToleranceSeconds == 0.002);

    clock.restart();
    CHECK_FALSE(clock.current());
    CHECK_FALSE(clock.mediaTime(1.0));
    clock.resetStats();
    CHECK(clock.stats().presented == 0);
}
