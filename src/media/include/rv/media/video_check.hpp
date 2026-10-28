#pragma once

// A clip checked without playing it (I5 step 7, ADR-0016 §8: `retroverse-cli media --check-videos`): a VideoDecoder,
// the screens' own, decodes a sample of it (its first seconds, a second from its middle, its last second) and its
// verdict is the decoder's: the failure it reports with ADR-0004's thresholds, or playable. A stream that ends after
// a seek (at the middle's or at the tail's) must have reached the video stream's declared duration, as the decoder's
// truncation rule asks of a whole pass (90% of it, less 0.5 s): a file cut short whose index is intact fails as
// truncated without being decoded whole, wherever it was cut. A short clip is decoded whole. GPU-free; the calling
// thread waits for the decoder's (polling its queue).

#include <rv/media/video_decoder.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace rv::media {

struct VideoCheckSettings {
    /// The first seconds decoded, then a stretch from the middle and the end; a clip whose stream declares no more than
    /// headSeconds + middleSeconds + tailSeconds + kWholeClipMarginSeconds is decoded whole.
    double headSeconds = 2.0;
    double middleSeconds = 1.0;
    double tailSeconds = 1.0;
    /// The frames are converted at this many rows at most (a small conversion: the check looks at the decoding).
    int maxHeight = 120;
    /// Wall-clock time a clip may take (a sleeping disk, a network share): past it, the check ends without a verdict.
    std::chrono::milliseconds timeout{60000};
    /// Read while waiting: true ends the check without a verdict.
    const std::atomic<bool>* cancel = nullptr;
};

/// A clip shorter than the sample by at most this much is decoded whole.
inline constexpr double kWholeClipMarginSeconds = 3.0;

/// What a check found.
struct VideoCheckResult {
    /// The check reached a verdict (false: timed out, cancelled, or the decoder could not start).
    bool completed = false;
    /// The verdict: None (playable) or the decoder's failure.
    DecodeFailure failure = DecodeFailure::None;
    /// The failure's message (FFmpeg's or the truncation's), or why the check did not complete.
    std::string detail;
    /// The stream, when the file opened.
    std::optional<StreamInfo> info;
    /// The decoder's counters (decode errors and damaged frames among them).
    DecoderStats stats;
    /// Frames read, and the latest frame's time (seconds into the clip), the clip decoded whole.
    std::uint64_t frames = 0;
    double lastFrameSeconds = 0.0;
    bool whole = false;
    /// Wall-clock seconds the check took.
    double seconds = 0.0;
};

/// Checks the clip `path` (see the file comment). Never throws; a file that cannot be decoded is a verdict, not an
/// error.
[[nodiscard]] VideoCheckResult checkVideo(const std::filesystem::path& path, const VideoCheckSettings& settings = {});

} // namespace rv::media
