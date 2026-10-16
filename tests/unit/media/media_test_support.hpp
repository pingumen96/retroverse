#pragma once

// Helpers of the rv_media tests: a directory for video fixtures under the build tree, clips written at test time
// (never committed), waiting for a decoder thread without racing it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <rv/media/test_video.hpp>
#include <rv/media/video_decoder.hpp>
#include <rv/media/video_frame.hpp>

namespace rv::test {

/// Longest wait for a decoder thread in the tests: generous, for a loaded machine.
inline constexpr std::chrono::milliseconds kDecoderTimeout{30000};

/// A unique directory under the build tree (next to the test executable, in test-media/), removed recursively on
/// destruction, once the threads of the stopped decoders have ended: the video fixtures of one test. Declare it before
/// the decoders and playbacks that read its files.
class MediaTestDir {
public:
    MediaTestDir();
    ~MediaTestDir();
    MediaTestDir(const MediaTestDir&) = delete;
    MediaTestDir& operator=(const MediaTestDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    /// path() / relative (UTF-8).
    [[nodiscard]] std::filesystem::path file(std::string_view relativeUtf8) const;

private:
    std::filesystem::path m_path;
};

/// A small clip: `width` x `height`, `frameRate` fps, `frames` frames.
[[nodiscard]] media::TestVideoSpec smallClip(int width = 160, int height = 120, int frameRate = 10, int frames = 10);

/// Writes `spec` to `dir`/`name` and requires success.
[[nodiscard]] std::filesystem::path writeClip(const MediaTestDir& dir, std::string_view name,
                                              const media::TestVideoSpec& spec);

/// Polls `condition` every millisecond until it holds (true) or `timeout` passes (false).
[[nodiscard]] bool waitUntil(const std::function<bool()>& condition,
                             std::chrono::milliseconds timeout = kDecoderTimeout);

/// What a test keeps of a decoded frame.
struct DecodedFrame {
    media::FrameTiming timing;
    std::uint64_t serial = 0;
    std::uint32_t loop = 0;
    media::FrameSize size;
    std::optional<std::uint32_t> counter;
};

/// Pops frames from `decoder` until `count` are collected, the decoder stops producing (failed, ended and drained), or
/// `timeout` passes.
[[nodiscard]] std::vector<DecodedFrame> collectFrames(media::VideoDecoder& decoder, std::size_t count,
                                                      std::chrono::milliseconds timeout = kDecoderTimeout);

/// Starts a decoder of `path` with `settings` and requires success.
[[nodiscard]] std::unique_ptr<media::VideoDecoder> startDecoder(const std::filesystem::path& path,
                                                                media::DecoderSettings settings = {});

} // namespace rv::test
