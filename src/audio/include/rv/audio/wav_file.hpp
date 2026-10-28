#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::audio {

/// A WAV file's samples as floats in [-1, 1], interleaved.
struct WavData {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;

    [[nodiscard]] std::size_t frames() const noexcept {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
};

/// Writes `interleaved` as a 32-bit float WAV (WAVE_FORMAT_IEEE_FLOAT) of `channels` channels at `sampleRate`, through a
/// temporary file renamed over `path`. Errors: InvalidArgument (no channel, a rate below 1, a count not a multiple of
/// the channels, more than 4 GiB of data), IoError.
[[nodiscard]] Status writeWavFile(const std::filesystem::path& path, std::span<const float> interleaved, int sampleRate,
                                  int channels);

/// Reads a WAV file of 32-bit float or 16-bit PCM samples. Errors: IoError, ParseError (not a WAV file, another
/// format, a chunk past the end).
[[nodiscard]] Expected<WavData> readWavFile(const std::filesystem::path& path);

} // namespace rv::audio
