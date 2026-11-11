#pragma once

#include <vector>

namespace rv::audio {

/// A mono sound held whole in memory (I12 step 7): a synthesized loop or one-shot the Mixer plays at its own rate,
/// which is the mixer's (no resampling).
struct PcmBuffer {
    std::vector<float> samples;
    int sampleRate = 48000;
    /// Played over and over (its last sample flows into its first), else once.
    bool loop = false;

    [[nodiscard]] double seconds() const noexcept {
        return sampleRate > 0 ? static_cast<double>(samples.size()) / static_cast<double>(sampleRate) : 0.0;
    }

    bool operator==(const PcmBuffer&) const = default;
};

} // namespace rv::audio
