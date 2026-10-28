// rv_audio's ring (I5 step 5, ADR-0016 §9): bounded at 0.5 s, timestamped in chunks, seek generations, never waiting.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <rv/audio/audio_ring.hpp>

using Catch::Approx;
using rv::audio::AudioRing;
using rv::audio::kMixRate;

namespace {

/// `frames` stereo frames whose left channel counts up from `first` and right channel is its negative.
std::vector<float> ramp(std::size_t frames, float first) {
    std::vector<float> samples(frames * 2);
    for (std::size_t index = 0; index < frames; ++index) {
        samples[index * 2] = first + static_cast<float>(index);
        samples[index * 2 + 1] = -(first + static_cast<float>(index));
    }
    return samples;
}

} // namespace

TEST_CASE("audio/ring: holds half a second at the mixer's rate and never more", "[audio][ring]") {
    AudioRing ring;
    CHECK(ring.sampleRate() == kMixRate);
    CHECK(ring.capacityFrames() == 24000);
    // A second written in 100 ms writes: the first half kept, the rest dropped and counted.
    std::size_t kept = 0;
    for (int write = 0; write < 10; ++write) {
        kept += ring.write(ramp(4800, static_cast<float>(write * 4800)), write * 0.1, 0);
        CHECK(ring.availableFrames() <= ring.capacityFrames());
    }
    CHECK(kept == 24000);
    CHECK(ring.availableFrames() == 24000);
    CHECK(ring.stats().overflowFrames == 24000);
    CHECK(ring.stats().framesWritten == 24000);
    // Reading makes room again; the next write keeps what fits.
    std::vector<float> out(1000 * 2);
    CHECK(ring.read(out.data(), 1000) == 1000);
    CHECK(out[0] == 0.0f);
    CHECK(out[999 * 2 + 1] == -999.0f);
    CHECK(ring.write(ramp(4800, 1.0f), 1.0, 0) == 1000);
    CHECK(ring.availableFrames() == 24000);
}

TEST_CASE("audio/ring: each chunk's time, the reader's position and the end", "[audio][ring]") {
    AudioRing ring(48000, 0.5);
    CHECK_FALSE(ring.front());
    CHECK_FALSE(ring.back());
    ring.write(ramp(480, 0.0f), 2.0, 3);
    ring.write(ramp(960, 480.0f), 2.01, 3);
    auto front = ring.front();
    REQUIRE(front);
    CHECK(front->ptsSeconds == Approx(2.0));
    CHECK(front->generation == 3);
    CHECK(front->frames == 480);
    const auto back = ring.back();
    REQUIRE(back);
    CHECK(back->ptsSeconds == Approx(2.03));
    // A read stops at its chunk's end; the next chunk's time follows.
    std::vector<float> out(2000 * 2);
    CHECK(ring.read(out.data(), 2000) == 480);
    front = ring.front();
    REQUIRE(front);
    CHECK(front->ptsSeconds == Approx(2.01));
    CHECK(ring.read(out.data(), 240) == 240);
    front = ring.front();
    REQUIRE(front);
    CHECK(front->ptsSeconds == Approx(2.015));
    CHECK(front->frames == 720);
    float frame[2] = {};
    REQUIRE(ring.peekFrame(frame));
    CHECK(frame[0] == 720.0f);
    CHECK(frame[1] == -720.0f);
    // skip() crosses chunks; clear() empties.
    CHECK(ring.skip(100) == 100);
    CHECK(ring.availableFrames() == 620);
    ring.clear();
    CHECK(ring.availableFrames() == 0);
    CHECK_FALSE(ring.front());
    CHECK(ring.stats().skippedFrames == 720);
}

TEST_CASE("audio/ring: a newer generation throws the older chunks away", "[audio][ring]") {
    AudioRing ring;
    ring.write(ramp(100, 0.0f), 5.0, 0);
    ring.write(ramp(100, 100.0f), 5.0 + 100.0 / 48000.0, 0);
    ring.write(ramp(50, 1000.0f), 1.0, 1);
    const auto front = ring.front(1);
    REQUIRE(front);
    CHECK(front->generation == 1);
    CHECK(front->ptsSeconds == Approx(1.0));
    CHECK(ring.availableFrames() == 50);
    CHECK(ring.stats().skippedFrames == 200);
    CHECK_FALSE(ring.front(2));
    CHECK(ring.availableFrames() == 0);
}

TEST_CASE("audio/ring: wraps around its end and stays exact under a concurrent writer", "[audio][ring]") {
    AudioRing ring(1000, 0.1); // 100 frames
    REQUIRE(ring.capacityFrames() == 100);
    std::atomic<bool> done{false};
    std::atomic<bool> shortWrite{false};
    constexpr std::size_t kTotal = 20000;
    // No Catch2 assertion on the writer's thread: it only records what went wrong.
    std::thread writer([&] {
        std::size_t written = 0;
        while (written < kTotal) {
            const std::size_t count = std::min<std::size_t>(37, kTotal - written);
            if (ring.availableFrames() + count <= ring.capacityFrames()) {
                if (ring.write(ramp(count, static_cast<float>(written)), static_cast<double>(written) / 1000.0, 0) !=
                    count) {
                    shortWrite = true;
                }
                written += count;
            } else {
                std::this_thread::yield();
            }
        }
        done = true;
    });
    std::size_t read = 0;
    float frame[2] = {};
    while (read < kTotal) {
        const auto front = ring.front();
        if (!front) {
            std::this_thread::yield();
            continue;
        }
        CHECK(front->ptsSeconds == Approx(static_cast<double>(read) / 1000.0));
        REQUIRE(ring.read(frame, 1) == 1);
        REQUIRE(frame[0] == static_cast<float>(read));
        REQUIRE(frame[1] == -static_cast<float>(read));
        ++read;
    }
    writer.join();
    CHECK(done);
    CHECK_FALSE(shortWrite);
    CHECK(ring.stats().overflowFrames == 0);
    CHECK(ring.stats().framesRead == kTotal);
}
