#include <rv/audio/spatial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace rv::audio {

double spatialGain(double distanceMeters, double proximityMeters) noexcept {
    if (!std::isfinite(distanceMeters) || !std::isfinite(proximityMeters) || proximityMeters <= 0.0 ||
        distanceMeters >= proximityMeters) {
        return 0.0;
    }
    const double distance = std::max(0.0, distanceMeters);
    const double falloff = distance <= kFullGainMeters ? 1.0 : kFullGainMeters / distance;
    const double fade = std::clamp((proximityMeters - distance) / kFadeMeters, 0.0, 1.0);
    return falloff * fade;
}

double panForAzimuth(double azimuthRadians) noexcept {
    if (!std::isfinite(azimuthRadians)) {
        return 0.0;
    }
    return std::clamp(std::sin(azimuthRadians), -kMaxPan, kMaxPan);
}

StereoGains equalPowerGains(double pan) noexcept {
    if (pan == 0.0 || !std::isfinite(pan)) {
        return StereoGains{1.0, 1.0}; // exactly unity: a centred source's samples pass unchanged
    }
    const double angle = (std::clamp(pan, -1.0, 1.0) + 1.0) * std::numbers::pi / 4.0;
    return StereoGains{std::cos(angle) * std::numbers::sqrt2, std::sin(angle) * std::numbers::sqrt2};
}

double decibelsFromGain(double gain) noexcept {
    return gain > 0.0 ? 20.0 * std::log10(gain) : -std::numeric_limits<double>::infinity();
}

double gainFromDecibels(double decibels) noexcept {
    return std::pow(10.0, decibels / 20.0);
}

} // namespace rv::audio
