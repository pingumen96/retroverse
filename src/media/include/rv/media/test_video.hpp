#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/media/video_frame.hpp>

// Synthetic video fixtures for tests and the video spike: clips encoded at test time with FFmpeg's own encoders (LGPL
// build, no external encoder library), never committed, and the damaged files of specification SC-08.

namespace rv::media {

/// Bits of the frame counter burned into every test video frame (frame numbers repeat every 4096 frames).
inline constexpr int kFrameCounterBits = 12;

/// Video codec of a test clip: FFmpeg's native encoders.
enum class TestVideoCodec : std::uint8_t {
    Mpeg4, ///< MPEG-4 Part 2 ("mpeg4"): .mp4, .mkv, .avi
    Mjpeg, ///< Motion JPEG ("mjpeg"): .mkv, .avi
};

[[nodiscard]] std::string_view toString(TestVideoCodec codec) noexcept;

/// A test clip.
struct TestVideoSpec {
    int width = 1280;
    int height = 720;
    /// Frames per second, 1..240.
    int frameRate = 30;
    /// 1..36000 frames' worth, seconds.
    double durationSeconds = 3.0;
    TestVideoCodec codec = TestVideoCodec::Mpeg4;
    /// Colors and motion of the pattern, and a letter A-Z drawn before the frame number.
    std::uint32_t seed = 0;
    /// Encoder quantizer, 2 (best) .. 31: MPEG-4 qscale, MJPEG quality.
    int quantizer = 3;
    /// Frames between key frames (MPEG-4).
    int keyFrameInterval = 30;
    /// B-frames between reference frames (MPEG-4): decoding then reorders, so timestamps come from the decoder.
    int bFrames = 2;
};

/// Writes a clip whose frames show a moving pattern, the frame number in large digits (the font of
/// rv::assets::makeSignLettering) and the same number as a row of kFrameCounterBits black or white blocks along the top
/// that decodeFrameCounter() reads back, so tests can check order, timestamps and seeks frame by frame. Frame k has
/// timestamp k / frameRate. The container follows the extension: ".mp4" (moov atom first, "faststart", so that a
/// truncated copy still opens and fails mid-stream), ".mkv" or ".avi". Errors: InvalidArgument for a spec out of range,
/// Unsupported for a codec the container or this FFmpeg build cannot take, IoError when the file cannot be written.
[[nodiscard]] Status writeTestVideo(const std::filesystem::path& path, const TestVideoSpec& spec);

/// The frame counter of a frame made by writeTestVideo(), read from the block row at any scale; nothing when a block
/// is neither clearly dark nor clearly bright (not a test frame, or damaged).
[[nodiscard]] std::optional<std::uint32_t> decodeFrameCounter(const VideoFrame& frame);

/// Copies the first `keepFraction` (0..1) of `source`'s bytes to `destination`: a file cut short mid-stream.
[[nodiscard]] Status writeTruncatedCopy(const std::filesystem::path& source, const std::filesystem::path& destination,
                                        double keepFraction);
/// Copies `source` with pseudo-random bytes (from `seed`) written over a part of every 16 KiB between 40% and 80% of
/// the file: a corrupt middle.
[[nodiscard]] Status writeCorruptedCopy(const std::filesystem::path& source, const std::filesystem::path& destination,
                                        std::uint32_t seed);
/// Copies an .avi `source` with its video codec's four-character codes replaced by "RVZZ", which no decoder knows: a
/// file with a video stream of an unsupported codec. Errors: ParseError when `source` holds no codec code to replace.
[[nodiscard]] Status writeUnknownCodecCopy(const std::filesystem::path& source,
                                           const std::filesystem::path& destination);
/// Writes `bytes` as is (an empty file, a text file named .mp4, ...).
[[nodiscard]] Status writeRawFile(const std::filesystem::path& path, std::string_view bytes);
/// Writes a silent 16-bit mono PCM .wav of `seconds` (0..60): a media file without a video stream.
[[nodiscard]] Status writeSilentWav(const std::filesystem::path& path, double seconds);

/// Names of the video encoders of the FFmpeg build linked in, sorted (diagnostics).
[[nodiscard]] std::vector<std::string> availableVideoEncoders();

} // namespace rv::media
