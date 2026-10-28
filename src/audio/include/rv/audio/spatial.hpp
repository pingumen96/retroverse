#pragma once

namespace rv::audio {

/// ADR-0016 §9's spatial rules: full gain within kFullGainMeters, then 1.5/d, fading to 0 over the last kFadeMeters
/// before proximityMeters; equal-power pan from the screen's azimuth, at most kMaxPan either side.
inline constexpr double kFullGainMeters = 1.5;
inline constexpr double kFadeMeters = 2.0;
inline constexpr double kMaxPan = 0.6;
/// The duck while the card, the directory or the quick catalog is open.
inline constexpr double kDuckDecibels = -9.0;

/// The gain of a screen `distanceMeters` from the eye: 1 within kFullGainMeters, then kFullGainMeters / d, times a
/// linear fade from 1 at proximityMeters - kFadeMeters to 0 at proximityMeters (0 beyond it, and for a proximity of 0).
[[nodiscard]] double spatialGain(double distanceMeters, double proximityMeters) noexcept;

/// The pan (-1 left .. +1 right) of a screen seen at `azimuthRadians` from the view's forward direction (positive to
/// the right, any angle): sin(azimuth), limited to +-kMaxPan, so that a screen behind is no harder to place than one
/// in front.
[[nodiscard]] double panForAzimuth(double azimuthRadians) noexcept;

/// The left and right gains of an equal-power pan: cos and sin of (pan + 1) * pi / 4, scaled by sqrt(2) so that the
/// centre is unity on both channels; left^2 + right^2 is 2 for every pan.
struct StereoGains {
    double left = 1.0;
    double right = 1.0;
};
[[nodiscard]] StereoGains equalPowerGains(double pan) noexcept;

[[nodiscard]] double decibelsFromGain(double gain) noexcept;
[[nodiscard]] double gainFromDecibels(double decibels) noexcept;

} // namespace rv::audio
