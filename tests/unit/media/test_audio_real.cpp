// A real 96 kHz SNES clip's sound against its picture (I5 step 5, ADR-0016 §9 and §12's acceptable row: within 80 ms
// at p95). Hidden ([.real-library]): ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only
// snapshots of the library before and after, RV_AUDIO=null and the media root in RV_REAL_MEDIA_ROOT. The snes system's
// videos folder is listed with its sub-folders (read only), the clips probed in name order until one carries 96 kHz
// sound, and that clip is played for 8 s of the wall clock as the application plays it: its VideoDecoder (480 rows,
// looping) decoding its sound on its own thread into a slot's ring, a VideoPlayback's presentation clock at 60 Hz, and
// the mixer's source following the picture's media time, its mix pulled each frame as the null output does. Each frame
// after the first second measures the media time of the sound being played against the timestamp of the frame shown.
// Nothing of the clip is printed but its stream's figures (the user's titles stay theirs).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <rv/audio/audio_ring.hpp>
#include <rv/audio/mixer.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/media/audio_sink.hpp>
#include <rv/media/video_decoder.hpp>
#include <rv/media/video_playback.hpp>

namespace fs = std::filesystem;

namespace {

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

/// The entry of `folder` named `name` in any letter case (ES-DE's folders keep the systems' spelling, users' may not).
std::optional<fs::path> childNamed(const fs::path& folder, std::string_view name) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (rv::core::equalsIgnoreCaseAscii(rv::core::pathToUtf8Lossy(it->path().filename()), name)) {
            return it->path();
        }
    }
    return std::nullopt;
}

/// The application's adapter (ScreenService's RingSink): the decoder's sound into the slot's ring.
class RingSink final : public rv::media::AudioSink {
public:
    explicit RingSink(std::shared_ptr<rv::audio::AudioRing> ring) : m_ring(std::move(ring)) {}
    void write(std::span<const float> interleaved, double ptsSeconds, std::uint32_t generation) override {
        static_cast<void>(m_ring->write(interleaved, ptsSeconds, generation));
    }

private:
    std::shared_ptr<rv::audio::AudioRing> m_ring;
};

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

} // namespace

TEST_CASE("media/audio: a real 96 kHz SNES clip's sound within 80 ms of its picture", "[media][audio][.real-library]") {
    const auto root = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!root) {
        SKIP("set RV_REAL_MEDIA_ROOT (scripts/real-library.ps1 does)");
    }
    const std::optional<fs::path> snes = childNamed(*root, "snes");
    const std::optional<fs::path> videos = snes ? childNamed(*snes, "videos") : std::nullopt;
    if (!videos) {
        SKIP("the media root has no snes/videos folder");
    }
    std::vector<fs::path> clips;
    std::error_code ec;
    // ES-DE's media folders may nest (a collection sorted into sub-folders): every .mp4 below.
    for (fs::recursive_directory_iterator it(*videos, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) &&
            rv::core::equalsIgnoreCaseAscii(rv::core::pathToUtf8Lossy(it->path().extension()), ".mp4")) {
            clips.push_back(it->path());
        }
    }
    std::sort(clips.begin(), clips.end());
    std::optional<fs::path> chosen;
    std::optional<rv::media::StreamInfo> info;
    std::size_t probed = 0;
    for (const fs::path& clip : clips) {
        if (++probed > 400) {
            break;
        }
        auto probe = rv::media::probeVideo(clip, 480);
        if (probe && probe->audioSampleRate == 96000 && probe->durationSeconds >= 10.0) {
            chosen = clip;
            info = *probe;
            break;
        }
    }
    if (!chosen) {
        SKIP("no 96 kHz clip of 10 s or more among the first " << probed << " of " << clips.size()
                                                                 << " snes clips");
    }

    rv::audio::Mixer mixer;
    const rv::audio::SourceId source = mixer.addSource();
    auto ring = std::make_shared<rv::audio::AudioRing>(mixer.settings().sampleRate);
    mixer.setRing(source, ring);
    rv::media::DecoderSettings settings;
    settings.maxHeight = 480;
    settings.loop = true;
    settings.name = "real-96k";
    settings.audioSink = std::make_shared<RingSink>(ring);
    settings.audioSampleRate = mixer.settings().sampleRate;
    auto decoder = rv::media::VideoDecoder::start(*chosen, settings);
    REQUIRE(decoder.has_value());
    rv::media::VideoPlayback playback(std::move(*decoder));

    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const auto seconds = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };
    std::vector<double> errors;
    std::vector<float> block;
    double pulledTo = 0.0;
    std::uint64_t frames = 0;
    for (int frame = 0;; ++frame) {
        const double now = seconds();
        if (now >= 8.0) {
            break;
        }
        const rv::media::PlaybackUpdate update = playback.update(now);
        if (update.frame != nullptr) {
            playback.releaseFrame(); // shown (nothing is uploaded here)
            ++frames;
        }
        const std::optional<double> media = playback.clock().mediaTime(now);
        mixer.setControl(source, rv::audio::SourceControl{1.0, 0.0, media.has_value()});
        mixer.setClock(source, rv::audio::SourceClock{media, now, playback.clock().generation(), true});
        // The null output's pull: the time since the last one.
        const auto count = static_cast<std::size_t>((now - pulledTo) * mixer.settings().sampleRate);
        if (count > 0) {
            block.resize(count * 2);
            mixer.render(block.data(), count, pulledTo);
            pulledTo += static_cast<double>(count) / mixer.settings().sampleRate;
        }
        // The sound played now (the next sample's media time) against the frame shown now.
        const std::optional<rv::media::FrameTiming>& shown = playback.clock().current();
        const std::optional<double> heard = mixer.sourceMediaTime(source);
        if (now >= 1.0 && shown && heard) {
            errors.push_back(std::abs(*heard - shown->ptsSeconds));
        }
        // 60 Hz.
        std::this_thread::sleep_until(start + std::chrono::microseconds(static_cast<long long>((frame + 1) * 16667)));
    }
    const rv::media::DecoderStats stats = playback.decoder().stats();
    const rv::audio::SyncSummary sync = mixer.syncSummary();
    const rv::audio::RingStats ringStats = ring->stats();
    playback.decoder().stop();
    rv::media::waitForStoppedDecoders();

    REQUIRE(errors.size() > 300);
    const double p50 = percentile(errors, 0.50) * 1000.0;
    const double p95 = percentile(errors, 0.95) * 1000.0;
    const double worst = *std::max_element(errors.begin(), errors.end()) * 1000.0;
    std::cout << "Real 96 kHz clip (I5 step 5): " << info->audioCodec << " " << info->audioSampleRate << " Hz, "
              << info->audioChannels << " channel(s), video " << info->codec << " " << info->codedSize.width << "x"
              << info->codedSize.height << " " << info->frameRate << " fps, " << info->durationSeconds
              << " s (the clip probed " << probed << " of " << clips.size() << "); 8 s played: " << frames
              << " frames shown, " << stats.audioFramesWritten << " audio frames decoded at 48 kHz; sound against "
              << "picture over " << errors.size() << " frames: p50 " << p50 << " ms, p95 " << p95 << " ms, max "
              << worst << " ms; the mixer's sync p95 " << sync.p95Milliseconds << " ms, " << sync.resyncs
              << " resync(s), " << sync.corrections << " corrected block(s), " << sync.underrunFrames
              << " underrun frame(s), ring overflow " << ringStats.overflowFrames << " frame(s), "
              << stats.audioDecodeErrors << " audio decode error(s)" << std::endl;
    CHECK(info->audioSampleRate == 96000);
    CHECK(stats.audioFramesWritten > 0);
    CHECK(p95 <= 80.0); // ADR-0016 §12: within 80 ms at p95
}
