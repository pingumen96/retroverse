#pragma once

#include <cstddef>
#include <span>
#include <vector>

// The tone analysis of the audio tests (unit and GPU smoke tests): one channel out of interleaved samples, the RMS
// level, a tone's amplitude by Goertzel, a tone's frequency by its zero crossings, the onsets of beeps.

namespace rv::audio {

/// Channel `channel` of `interleaved` (`channels` channels).
[[nodiscard]] std::vector<float> channelOf(std::span<const float> interleaved, int channels, int channel);
/// The mean power of both channels of interleaved stereo, frame by frame: (L^2 + R^2) / 2, per frame.
[[nodiscard]] std::vector<float> stereoPower(std::span<const float> interleaved);

/// The RMS of `samples` (0 for none).
[[nodiscard]] double rmsOf(std::span<const float> samples);
/// The largest |sample|.
[[nodiscard]] double peakOf(std::span<const float> samples);

/// The amplitude (peak) of a sinusoid at `frequency` in `samples` at `sampleRate`, by Goertzel's algorithm over a Hann
/// window: a full-scale sine at that frequency gives about 1, a tone elsewhere far below (its leakage).
[[nodiscard]] double toneAmplitude(std::span<const float> samples, double frequency, int sampleRate);

/// The frequency of the tone in `samples`, from its first to its last upward zero crossing (linearly interpolated):
/// precise to a small fraction of a hertz over a second of a clean tone. 0 without two crossings.
[[nodiscard]] double toneFrequency(std::span<const float> samples, int sampleRate);

/// The times (seconds from the first sample) where a sound starts after at least `quietSeconds` of samples whose |x|
/// stays below `threshold`: the first sample above `threshold` after such a gap (and the first of all, after quiet
/// from the start).
[[nodiscard]] std::vector<double> onsets(std::span<const float> samples, int sampleRate, double threshold,
                                         double quietSeconds);

} // namespace rv::audio
