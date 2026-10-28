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

std::optional<std::size_t> chooseAudible(std::span<const AudibleCandidate> candidates, double proximityMeters,
                                         std::string_view current) {
    const auto qualifies = [proximityMeters](const AudibleCandidate& candidate) {
        return candidate.visible && candidate.playing && candidate.distance < proximityMeters;
    };
    std::optional<std::size_t> best;
    std::optional<std::size_t> kept;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const AudibleCandidate& candidate = candidates[index];
        if (!qualifies(candidate)) {
            continue;
        }
        if (!current.empty() && candidate.screen == current) {
            kept = index;
        }
        if (!best) {
            best = index;
            continue;
        }
        const AudibleCandidate& leader = candidates[*best];
        if (candidate.screenClass != leader.screenClass) {
            if (candidate.screenClass < leader.screenClass) {
                best = index;
            }
        } else if (candidate.distance != leader.distance) {
            if (candidate.distance < leader.distance) {
                best = index;
            }
        } else if (candidate.screen < leader.screen) {
            best = index;
        }
    }
    if (!best || !kept || *best == *kept) {
        return best;
    }
    // The screen heard so far keeps the sound unless a better class appears, or one of its class is clearly nearer.
    const AudibleCandidate& challenger = candidates[*best];
    const AudibleCandidate& heard = candidates[*kept];
    if (challenger.screenClass < heard.screenClass ||
        (challenger.screenClass == heard.screenClass &&
         challenger.distance <= heard.distance - kAudibleHysteresisMeters)) {
        return best;
    }
    return kept;
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
