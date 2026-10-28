#include <rv/audio/analysis.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace rv::audio {

std::vector<float> channelOf(std::span<const float> interleaved, int channels, int channel) {
    std::vector<float> out;
    if (channels < 1 || channel < 0 || channel >= channels) {
        return out;
    }
    const auto stride = static_cast<std::size_t>(channels);
    out.reserve(interleaved.size() / stride);
    for (std::size_t index = static_cast<std::size_t>(channel); index < interleaved.size(); index += stride) {
        out.push_back(interleaved[index]);
    }
    return out;
}

std::vector<float> stereoPower(std::span<const float> interleaved) {
    std::vector<float> out(interleaved.size() / 2);
    for (std::size_t index = 0; index < out.size(); ++index) {
        const float left = interleaved[index * 2];
        const float right = interleaved[index * 2 + 1];
        out[index] = 0.5f * (left * left + right * right);
    }
    return out;
}

double rmsOf(std::span<const float> samples) {
    if (samples.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const float sample : samples) {
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

double peakOf(std::span<const float> samples) {
    double peak = 0.0;
    for (const float sample : samples) {
        peak = std::max(peak, std::abs(static_cast<double>(sample)));
    }
    return peak;
}

double toneAmplitude(std::span<const float> samples, double frequency, int sampleRate) {
    const std::size_t count = samples.size();
    if (count < 2 || sampleRate < 1) {
        return 0.0;
    }
    const double omega = 2.0 * std::numbers::pi * frequency / static_cast<double>(sampleRate);
    const double coefficient = 2.0 * std::cos(omega);
    double previous = 0.0;
    double before = 0.0;
    double windowSum = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        const double window =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) / static_cast<double>(count - 1));
        windowSum += window;
        const double value = static_cast<double>(samples[index]) * window + coefficient * previous - before;
        before = previous;
        previous = value;
    }
    const double real = previous - before * std::cos(omega);
    const double imaginary = before * std::sin(omega);
    return 2.0 * std::sqrt(real * real + imaginary * imaginary) / windowSum;
}

double toneFrequency(std::span<const float> samples, int sampleRate) {
    std::optional<double> first;
    double last = 0.0;
    std::size_t crossings = 0;
    for (std::size_t index = 1; index < samples.size(); ++index) {
        const double a = samples[index - 1];
        const double b = samples[index];
        if (a < 0.0 && b >= 0.0) {
            const double at = static_cast<double>(index - 1) + (-a) / (b - a);
            if (!first) {
                first = at;
            } else {
                last = at;
                ++crossings;
            }
        }
    }
    if (!first || crossings == 0 || last <= *first) {
        return 0.0;
    }
    return static_cast<double>(crossings) * static_cast<double>(sampleRate) / (last - *first);
}

std::vector<double> onsets(std::span<const float> samples, int sampleRate, double threshold, double quietSeconds) {
    std::vector<double> found;
    const auto quiet = static_cast<std::size_t>(std::max(0.0, quietSeconds) * static_cast<double>(sampleRate));
    std::size_t quietRun = quiet; // quiet before the first sample
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (std::abs(static_cast<double>(samples[index])) >= threshold) {
            if (quietRun >= quiet) {
                found.push_back(static_cast<double>(index) / static_cast<double>(sampleRate));
            }
            quietRun = 0;
        } else {
            ++quietRun;
        }
    }
    return found;
}

} // namespace rv::audio
