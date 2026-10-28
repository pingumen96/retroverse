#include <rv/app/audio_policy.hpp>

#include <rv/audio/spatial.hpp>

#include <cmath>

namespace rv::app {

AudioOutputChoice resolveAudioOutput(std::optional<audio::AudioOutput> commandLine,
                                     std::optional<std::string_view> environment, bool frameLimited) {
    if (environment) {
        if (const std::optional<audio::AudioOutput> forced = audio::audioOutputFromName(*environment);
            forced && *forced != audio::AudioOutput::Device) {
            return AudioOutputChoice{*forced, "RV_AUDIO"};
        }
    }
    if (commandLine) {
        return AudioOutputChoice{*commandLine, "--audio"};
    }
    if (frameLimited) {
        return AudioOutputChoice{audio::AudioOutput::Null, "frame-limited"};
    }
    return AudioOutputChoice{audio::AudioOutput::Device, "interactive"};
}

std::optional<std::size_t> chooseAudible(std::span<const AudibleCandidate> candidates, double proximityMeters) {
    std::optional<std::size_t> best;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const AudibleCandidate& candidate = candidates[index];
        if (!candidate.visible || !candidate.playing || !(candidate.distance < proximityMeters)) {
            continue;
        }
        if (!best) {
            best = index;
            continue;
        }
        const AudibleCandidate& current = candidates[*best];
        if (candidate.screenClass != current.screenClass) {
            if (candidate.screenClass < current.screenClass) {
                best = index;
            }
        } else if (candidate.distance != current.distance) {
            if (candidate.distance < current.distance) {
                best = index;
            }
        } else if (candidate.screen < current.screen) {
            best = index;
        }
    }
    return best;
}

SpatialSound spatialSound(double distanceMeters, double azimuthRadians, double proximityMeters) noexcept {
    return SpatialSound{audio::spatialGain(distanceMeters, proximityMeters), audio::panForAzimuth(azimuthRadians)};
}

double azimuthOf(double eyeX, double eyeZ, double forwardX, double forwardZ, double targetX, double targetZ) noexcept {
    const double toX = targetX - eyeX;
    const double toZ = targetZ - eyeZ;
    if ((forwardX == 0.0 && forwardZ == 0.0) || (toX == 0.0 && toZ == 0.0)) {
        return 0.0;
    }
    // Right of a horizontal forward (x, z) is (-z, x): looking along -Z, right is +X.
    const double ahead = toX * forwardX + toZ * forwardZ;
    const double right = toX * -forwardZ + toZ * forwardX;
    return std::atan2(right, ahead);
}

} // namespace rv::app
