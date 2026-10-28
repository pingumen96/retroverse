#include <rv/audio/mixer.hpp>

#include <rv/audio/spatial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace rv::audio {
namespace {

/// The sync's measures kept (each block's error): about 5 hours of 60 Hz blocks.
constexpr std::size_t kMaxErrorSamples = 1u << 20;

double stepFor(double seconds, int sampleRate) noexcept {
    return seconds > 0.0 ? 1.0 / (seconds * static_cast<double>(sampleRate)) : 1.0;
}

double percentile(std::vector<float> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    const auto index = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    const auto nth = values.begin() + static_cast<std::ptrdiff_t>(std::min(index, values.size() - 1));
    std::nth_element(values.begin(), nth, values.end());
    return static_cast<double>(*nth);
}

} // namespace

double Mixer::Ramp::advance() noexcept {
    if (current < target) {
        current = std::min(target, current + step);
    } else if (current > target) {
        current = std::max(target, current - step);
    }
    return current;
}

Mixer::Mixer(MixerSettings settings) : m_settings(settings) {
    m_settings.sampleRate = std::max(1, m_settings.sampleRate);
    const int rate = m_settings.sampleRate;
    m_master = Ramp{1.0, 1.0, stepFor(m_settings.muteFadeSeconds, rate)};
    m_duck = Ramp{1.0, 1.0, stepFor(m_settings.duckFadeSeconds, rate)};
    m_mute = Ramp{1.0, 1.0, stepFor(m_settings.muteFadeSeconds, rate)};
    m_silence = Ramp{1.0, 1.0, stepFor(m_settings.silenceFadeSeconds, rate)};
}

SourceId Mixer::addSource() {
    const std::scoped_lock lock(m_mutex);
    Source source;
    source.gain = Ramp{0.0, 0.0, stepFor(m_settings.crossFadeSeconds, m_settings.sampleRate)};
    m_sources.push_back(std::move(source));
    return static_cast<SourceId>(m_sources.size() - 1);
}

void Mixer::setRing(SourceId id, std::shared_ptr<AudioRing> ring) {
    const std::scoped_lock lock(m_mutex);
    if (id >= m_sources.size()) {
        return;
    }
    Source& source = m_sources[id];
    source.ring = std::move(ring);
    source.gain.current = 0.0;
    source.hasPrevious = false;
    source.fraction = 0.0;
    source.needResync = true;
    source.sync.reset();
    source.clock = SourceClock{};
}

void Mixer::setControl(SourceId id, const SourceControl& control) {
    const std::scoped_lock lock(m_mutex);
    if (id < m_sources.size()) {
        Source& source = m_sources[id];
        source.control = control;
        source.control.gain = std::isfinite(control.gain) ? std::max(0.0, control.gain) : 0.0;
        source.control.pan = std::isfinite(control.pan) ? std::clamp(control.pan, -1.0, 1.0) : 0.0;
        // Paused: down within the pause fade; otherwise the cross-fade's slope.
        source.gain.step =
            stepFor(control.playing ? m_settings.crossFadeSeconds : m_settings.pauseFadeSeconds, m_settings.sampleRate);
        source.gain.target = control.playing ? source.control.gain : 0.0;
    }
}

void Mixer::setClock(SourceId id, const SourceClock& clock) {
    const std::scoped_lock lock(m_mutex);
    if (id < m_sources.size()) {
        m_sources[id].clock = clock;
    }
}

void Mixer::setMaster(double gain) {
    const std::scoped_lock lock(m_mutex);
    m_master.target = std::isfinite(gain) ? std::clamp(gain, 0.0, 1.0) : 1.0;
}

void Mixer::setDucked(bool ducked) {
    const std::scoped_lock lock(m_mutex);
    m_ducked = ducked;
    m_duck.target = ducked ? gainFromDecibels(kDuckDecibels) : 1.0;
}

void Mixer::setMuted(bool muted) {
    const std::scoped_lock lock(m_mutex);
    m_muted = muted;
    m_mute.target = muted ? 0.0 : 1.0;
}

void Mixer::setSilenced(bool silenced) {
    const std::scoped_lock lock(m_mutex);
    m_silence.target = silenced ? 0.0 : 1.0;
}

bool Mixer::muted() const {
    const std::scoped_lock lock(m_mutex);
    return m_muted;
}

bool Mixer::ducked() const {
    const std::scoped_lock lock(m_mutex);
    return m_ducked;
}

bool Mixer::consumeOne(Source& source) {
    const std::optional<RingFront> front = source.ring->front(source.clock.generation);
    if (!front) {
        return false;
    }
    float frame[2];
    if (source.ring->read(frame, 1) != 1) {
        return false;
    }
    source.previous[0] = frame[0];
    source.previous[1] = frame[1];
    source.previousTime = front->ptsSeconds;
    source.generation = front->generation;
    source.hasPrevious = true;
    return true;
}

bool Mixer::peekNext(Source& source, float* next) {
    return source.ring->front(source.clock.generation).has_value() && source.ring->peekFrame(next);
}

void Mixer::resync(Source& source, double target, std::size_t frames, std::size_t& index) {
    const double rate = static_cast<double>(m_settings.sampleRate);
    source.hasPrevious = false;
    source.fraction = 0.0;
    source.sync.reset();
    // The sound behind the picture: drop up to the picture's time.
    for (;;) {
        const std::optional<RingFront> front = source.ring->front(source.clock.generation);
        if (!front) {
            return; // nothing yet: silence, still to resync
        }
        const double behind = target - front->ptsSeconds;
        const auto drop = static_cast<std::size_t>(std::max(0.0, std::floor(behind * rate + 0.5)));
        if (drop == 0) {
            break;
        }
        if (source.ring->skip(std::min(drop, front->frames)) == 0) {
            return;
        }
    }
    // The sound ahead: silence until its time.
    const std::optional<RingFront> front = source.ring->front(source.clock.generation);
    const double ahead = front->ptsSeconds - target;
    const auto pad = static_cast<std::size_t>(std::max(0.0, std::floor(ahead * rate + 0.5)));
    index = std::min(frames, index + pad);
    if (index < frames && consumeOne(source)) {
        source.needResync = false;
    }
}

void Mixer::recordError(double seconds) {
    ++m_counts.blocks;
    if (m_errorsMs.size() < kMaxErrorSamples) {
        m_errorsMs.push_back(static_cast<float>(std::abs(seconds) * 1000.0));
    }
}

bool Mixer::renderSource(Source& source, std::size_t frames, double blockStartSeconds) {
    float* scratch = m_scratch.data();
    std::fill(scratch, scratch + frames * kMixChannels, 0.0f);
    if (!source.ring) {
        source.gain.current = 0.0;
        return false;
    }
    const bool fading = source.gain.current > 0.0;
    if (!source.control.playing && !fading) {
        // Paused (or stopped) and silent: nothing is read; the next play starts at the picture's time.
        source.needResync = true;
        return false;
    }
    if (source.control.playing && source.control.gain <= 0.0 && !fading) {
        // Not the audible screen: whatever its decoder still wrote is never played.
        source.ring->clear();
        source.needResync = true;
        return false;
    }
    if (!source.clock.mediaSeconds) {
        return false; // no picture yet
    }
    const double rate = static_cast<double>(m_settings.sampleRate);
    const double target =
        *source.clock.mediaSeconds + (source.clock.running ? blockStartSeconds - source.clock.atSeconds : 0.0);
    std::size_t index = 0;
    double ratio = 1.0;
    if (source.hasPrevious && source.generation < source.clock.generation) {
        source.needResync = true; // a seek: the sound of the old position stops
    }
    if (source.control.playing && source.clock.running) {
        if (source.needResync || !source.hasPrevious) {
            // A start: a new clip, a seek, a resume, the ring run dry.
            resync(source, target, frames, index);
        } else {
            const double error = source.previousTime + source.fraction / rate - target;
            if (source.gain.current > 0.0) {
                recordError(error);
            }
            const SyncDecision decision = source.sync.update(error);
            if (decision.action == SyncAction::Resync) {
                ++m_counts.resyncs; // in play (a start is no resync)
                resync(source, target, frames, index);
            } else {
                ratio = decision.ratio;
                if (decision.action == SyncAction::Correct) {
                    ++m_counts.corrections;
                }
            }
        }
    }
    if (!source.hasPrevious) {
        if (source.control.playing && source.gain.target > 0.0) {
            m_counts.underrunFrames += frames - index;
        }
        return false;
    }
    float next[2];
    for (; index < frames; ++index) {
        if (source.fraction == 0.0) {
            // On a sample: copied exactly.
            scratch[index * 2] = source.previous[0];
            scratch[index * 2 + 1] = source.previous[1];
        } else if (peekNext(source, next)) {
            const auto fraction = static_cast<float>(source.fraction);
            scratch[index * 2] = source.previous[0] + fraction * (next[0] - source.previous[0]);
            scratch[index * 2 + 1] = source.previous[1] + fraction * (next[1] - source.previous[1]);
        } else {
            scratch[index * 2] = source.previous[0];
            scratch[index * 2 + 1] = source.previous[1];
        }
        source.fraction += ratio;
        bool starved = false;
        while (source.fraction >= 1.0) {
            source.fraction -= 1.0;
            if (!consumeOne(source)) {
                starved = true;
                break;
            }
        }
        if (starved) {
            // The ring ran dry: silence until the decoder catches up, then a resync to the picture's time.
            source.hasPrevious = false;
            source.fraction = 0.0;
            source.needResync = true;
            if (source.control.playing && source.gain.target > 0.0) {
                m_counts.underrunFrames += frames - index - 1;
            }
            break;
        }
    }
    return true;
}

void Mixer::render(float* out, std::size_t frames, double blockStartSeconds) {
    const std::scoped_lock lock(m_mutex);
    std::fill(out, out + frames * kMixChannels, 0.0f);
    if (m_scratch.size() < frames * kMixChannels) {
        m_scratch.resize(frames * kMixChannels);
    }
    for (Source& source : m_sources) {
        // A source rises only while it sounds: its fade-in starts with its first sample, not with its request.
        const bool sounding = renderSource(source, frames, blockStartSeconds);
        const bool hold = !sounding && source.gain.target > source.gain.current;
        const StereoGains pan = equalPowerGains(source.control.pan);
        for (std::size_t index = 0; index < frames; ++index) {
            const double gain = hold ? source.gain.current : source.gain.advance();
            if (gain == 0.0) {
                continue;
            }
            out[index * 2] += static_cast<float>(static_cast<double>(m_scratch[index * 2]) * gain * pan.left);
            out[index * 2 + 1] += static_cast<float>(static_cast<double>(m_scratch[index * 2 + 1]) * gain * pan.right);
        }
    }
    for (std::size_t index = 0; index < frames; ++index) {
        const double gain = m_master.advance() * m_duck.advance() * m_mute.advance() * m_silence.advance();
        for (int channel = 0; channel < kMixChannels; ++channel) {
            float& sample = out[index * 2 + static_cast<std::size_t>(channel)];
            sample = static_cast<float>(std::clamp(static_cast<double>(sample) * gain, -1.0, 1.0));
        }
    }
    m_framesRendered += frames;
}

double Mixer::sourceGain(SourceId id) const {
    const std::scoped_lock lock(m_mutex);
    return id < m_sources.size() ? m_sources[id].gain.current : 0.0;
}

std::optional<double> Mixer::sourceMediaTime(SourceId id) const {
    const std::scoped_lock lock(m_mutex);
    if (id >= m_sources.size() || !m_sources[id].hasPrevious) {
        return std::nullopt;
    }
    const Source& source = m_sources[id];
    return source.previousTime + source.fraction / static_cast<double>(m_settings.sampleRate);
}

double Mixer::outputGain() const {
    const std::scoped_lock lock(m_mutex);
    return m_master.current * m_duck.current * m_mute.current * m_silence.current;
}

SyncSummary Mixer::syncSummary() const {
    const std::scoped_lock lock(m_mutex);
    SyncSummary summary = m_counts;
    summary.p50Milliseconds = percentile(m_errorsMs, 0.50);
    summary.p95Milliseconds = percentile(m_errorsMs, 0.95);
    summary.maxMilliseconds =
        m_errorsMs.empty() ? 0.0 : static_cast<double>(*std::max_element(m_errorsMs.begin(), m_errorsMs.end()));
    return summary;
}

std::uint64_t Mixer::framesRendered() const {
    const std::scoped_lock lock(m_mutex);
    return m_framesRendered;
}

} // namespace rv::audio
