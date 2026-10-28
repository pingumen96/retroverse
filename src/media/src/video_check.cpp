#include <rv/media/video_check.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

namespace rv::media {
namespace {

using Clock = std::chrono::steady_clock;

/// How often the waiting thread looks at the decoder's queue and state.
constexpr std::chrono::milliseconds kPoll{1};
/// Frames decoded ahead: the decoder runs while the checking thread sleeps.
constexpr std::size_t kCheckQueue = 16;

/// How a stretch of the sample ended.
enum class Stretch : std::uint8_t {
    Reached, ///< a frame at or past its end was read
    Ended,   ///< the stream ended (every frame read)
    Failed,  ///< the decoder failed
    Stopped, ///< timed out or cancelled
};

class Checker {
public:
    Checker(VideoDecoder& decoder, const VideoCheckSettings& settings, VideoCheckResult& result)
        : m_decoder(decoder), m_settings(settings), m_result(result), m_deadline(Clock::now() + settings.timeout) {}

    /// Reads frames of generation `generation` or later until one at or past `untilSeconds`, the stream's end, a
    /// failure or the time limit.
    Stretch read(double untilSeconds, std::uint32_t generation) {
        while (true) {
            while (PooledFrame frame = m_decoder.frames().pop()) {
                if (frame->timing.generation < generation) {
                    continue; // a frame of the position before the seek
                }
                ++m_result.frames;
                m_result.lastFrameSeconds = frame->timing.ptsSeconds;
                if (frame->timing.ptsSeconds >= untilSeconds) {
                    return Stretch::Reached;
                }
            }
            const DecoderState state = m_decoder.state();
            if (state == DecoderState::Failed) {
                return Stretch::Failed;
            }
            if (state == DecoderState::Ended && m_decoder.frames().empty() && m_decoder.generation() == generation) {
                return Stretch::Ended;
            }
            if (Clock::now() >= m_deadline || (m_settings.cancel != nullptr && m_settings.cancel->load())) {
                return Stretch::Stopped;
            }
            std::this_thread::sleep_for(kPoll);
        }
    }

    /// Waits until the stream is open (or the decoder failed, or the time is up).
    bool waitOpen() {
        while (m_decoder.state() == DecoderState::Opening) {
            if (Clock::now() >= m_deadline || (m_settings.cancel != nullptr && m_settings.cancel->load())) {
                return false;
            }
            std::this_thread::sleep_for(kPoll);
        }
        return true;
    }

    [[nodiscard]] Clock::time_point deadline() const noexcept { return m_deadline; }

private:
    VideoDecoder& m_decoder;
    const VideoCheckSettings& m_settings;
    VideoCheckResult& m_result;
    Clock::time_point m_deadline;
};

} // namespace

VideoCheckResult checkVideo(const std::filesystem::path& path, const VideoCheckSettings& settings) {
    const Clock::time_point started = Clock::now();
    VideoCheckResult result;
    DecoderSettings decoderSettings;
    decoderSettings.maxHeight = std::max(0, settings.maxHeight);
    decoderSettings.loop = false;
    decoderSettings.queueCapacity = kCheckQueue;
    // The file name names it in the log (the default), so that a clip that troubles the decoder can be found.
    auto opened = VideoDecoder::start(path, std::move(decoderSettings));
    if (!opened) {
        result.detail = opened.error().toString();
        return result;
    }
    VideoDecoder& decoder = **opened;
    Checker checker(decoder, settings, result);

    const auto finish = [&](Stretch stretch) {
        if (stretch == Stretch::Failed) {
            result.completed = true;
            result.failure = decoder.failure();
            if (const std::optional<Error> error = decoder.error()) {
                result.detail = error->message;
            }
        } else if (stretch == Stretch::Stopped) {
            result.completed = false;
            result.detail = settings.cancel != nullptr && settings.cancel->load()
                                ? std::string("cancelled")
                                : fmt::format("not done within {} s", settings.timeout.count() / 1000.0);
        } else {
            result.completed = true;
        }
        result.info = decoder.streamInfo();
        result.stats = decoder.stats();
        decoder.stop();
        // The decoder's thread ends before the next clip's starts: a scan runs as many decoder threads as jobs.
        while (decoder.threadRunning() && Clock::now() < checker.deadline() + std::chrono::seconds(5)) {
            std::this_thread::sleep_for(kPoll);
        }
        result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    };

    if (!checker.waitOpen()) {
        finish(Stretch::Stopped);
        return result;
    }
    if (decoder.state() == DecoderState::Failed) {
        finish(Stretch::Failed);
        return result;
    }
    const std::optional<StreamInfo> info = decoder.streamInfo();
    const double declared = info ? info->durationSeconds : 0.0;
    const double sample = settings.headSeconds + settings.middleSeconds + settings.tailSeconds;
    if (!(declared > sample + kWholeClipMarginSeconds)) {
        // A short clip (or one that declares no duration) is decoded whole: the decoder's own first pass applies the
        // truncation rule.
        result.whole = true;
        finish(checker.read(std::numeric_limits<double>::infinity(), 0));
        return result;
    }

    Stretch stretch = checker.read(settings.headSeconds, 0);
    if (stretch != Stretch::Reached) {
        finish(stretch);
        return result;
    }
    const double middle = std::max(settings.headSeconds, declared / 2.0 - settings.middleSeconds / 2.0);
    stretch = checker.read(middle + settings.middleSeconds, decoder.seek(middle));
    if (stretch != Stretch::Reached) {
        finish(stretch);
        return result;
    }
    stretch = checker.read(std::numeric_limits<double>::infinity(), decoder.seek(declared - settings.tailSeconds));
    finish(stretch);
    if (stretch == Stretch::Ended) {
        // The sample's last frame against the stream's own duration (never the container's, which covers a longer
        // sound): ADR-0004's truncation rule.
        const double own = result.info ? result.info->streamDurationSeconds : 0.0;
        if (own > 0.0 && result.lastFrameSeconds < own * kTruncatedPassRatio - kTruncatedPassSlackSeconds) {
            result.failure = DecodeFailure::Truncated;
            result.detail =
                fmt::format("the stream ends after {:.2f} s of the {:.2f} s it declares", result.lastFrameSeconds, own);
        }
    }
    return result;
}

} // namespace rv::media
