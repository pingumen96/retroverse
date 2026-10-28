#include <rv/audio/mixer.hpp>

#include <rv/audio/spatial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace rv::audio {
namespace {

/// The sync's error histogram: bins of 10 us up to 200 ms, then one bin for everything beyond.
constexpr double kErrorBinMilliseconds = 0.01;
constexpr std::size_t kErrorBins = 20000;

double stepFor(double seconds, int sampleRate) noexcept {
    return seconds > 0.0 ? 1.0 / (seconds * static_cast<double>(sampleRate)) : 1.0;
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
    // Sized once: render() never allocates (it runs on the device's thread, under the mutex).
    m_scratch.assign(kMaxBlockFrames * kMixChannels, 0.0f);
    m_errorBins.assign(kErrorBins + 1, 0);
}

SourceId Mixer::addSource() {
    const std::scoped_lock lock(m_mutex);
    Source source;
    source.gain = Ramp{0.0, 0.0, stepFor(m_settings.crossFadeSeconds, m_settings.sampleRate)};
    source.pan = Ramp{0.0, 0.0, stepFor(m_settings.panSlewSeconds, m_settings.sampleRate)};
    m_sources.push_back(std::move(source));
    return static_cast<SourceId>(m_sources.size() - 1);
}

void Mixer::setRing(SourceId id, std::shared_ptr<AudioRing> ring) {
    const std::scoped_lock lock(m_mutex);
    if (id >= m_sources.size()) {
        return;
    }
    Source& source = m_sources[id];
    if (source.ring && source.gain.current > 0.0) {
        // Still sounding: the old clip fades out first (the pause's fade, on its own clock), then the new one starts.
        source.pendingRing = std::move(ring);
        source.pendingClock = SourceClock{};
        source.ringPending = true;
        retarget(source);
        return;
    }
    installRing(source, std::move(ring), SourceClock{});
}

void Mixer::installRing(Source& source, std::shared_ptr<AudioRing> ring, const SourceClock& clock) {
    // The old ring is kept aside, not freed here: render() installs a pending ring on the device's thread, and the
    // main thread lets the old one go (setControl()).
    source.retiredRing = std::move(source.ring);
    source.ring = std::move(ring);
    source.ringPending = false;
    source.pendingRing.reset();
    source.gain.current = 0.0;
    source.hasPrevious = false;
    source.fraction = 0.0;
    source.needResync = true;
    source.sync.reset();
    source.clock = clock;
    retarget(source);
}

void Mixer::retarget(Source& source) {
    if (source.ringPending) {
        source.gain.step = stepFor(m_settings.pauseFadeSeconds, m_settings.sampleRate);
        source.gain.target = 0.0;
        return;
    }
    // Paused: down within the pause fade; otherwise the cross-fade's slope.
    const bool playing = source.control.playing;
    source.gain.step =
        stepFor(playing ? m_settings.crossFadeSeconds : m_settings.pauseFadeSeconds, m_settings.sampleRate);
    source.gain.target = playing ? source.control.gain : 0.0;
}

void Mixer::setControl(SourceId id, const SourceControl& control) {
    std::shared_ptr<AudioRing> retired;
    {
        const std::scoped_lock lock(m_mutex);
        if (id < m_sources.size()) {
            Source& source = m_sources[id];
            source.control = control;
            source.control.gain = std::isfinite(control.gain) ? std::max(0.0, control.gain) : 0.0;
            source.control.pan = std::isfinite(control.pan) ? std::clamp(control.pan, -1.0, 1.0) : 0.0;
            retarget(source);
            source.pan.target = source.control.pan;
            retired = std::move(source.retiredRing);
        }
    }
    // A ring the source no longer reads is freed here, on the main thread and outside the lock.
}

void Mixer::setClock(SourceId id, const SourceClock& clock) {
    const std::scoped_lock lock(m_mutex);
    if (id < m_sources.size()) {
        Source& source = m_sources[id];
        // While the old clip fades out it keeps its own clock; this one is the next clip's.
        (source.ringPending ? source.pendingClock : source.clock) = clock;
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
    const double milliseconds = std::abs(seconds) * 1000.0;
    if (!std::isfinite(milliseconds)) {
        ++m_errorBins[kErrorBins];
        return;
    }
    m_errorMaxMilliseconds = std::max(m_errorMaxMilliseconds, milliseconds);
    const double bin = std::floor(milliseconds / kErrorBinMilliseconds);
    ++m_errorBins[bin < static_cast<double>(kErrorBins) ? static_cast<std::size_t>(bin) : kErrorBins];
}

double Mixer::errorPercentile(double fraction) const {
    std::uint64_t count = 0;
    for (const std::uint64_t bin : m_errorBins) {
        count += bin;
    }
    if (count == 0) {
        return 0.0;
    }
    // The rank of the nearest-rank percentile; its bin's upper edge, never above the largest error measured.
    const auto rank = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(count))));
    std::uint64_t seen = 0;
    for (std::size_t bin = 0; bin < kErrorBins; ++bin) {
        seen += m_errorBins[bin];
        if (seen >= rank) {
            return std::min(static_cast<double>(bin + 1) * kErrorBinMilliseconds, m_errorMaxMilliseconds);
        }
    }
    return m_errorMaxMilliseconds;
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
    // A block longer than the scratch (a null output's pull after a long frame) is rendered in pieces.
    const double rate = static_cast<double>(m_settings.sampleRate);
    for (std::size_t done = 0; done < frames;) {
        const std::size_t piece = std::min(kMaxBlockFrames, frames - done);
        renderBlock(out + done * kMixChannels, piece, blockStartSeconds + static_cast<double>(done) / rate);
        done += piece;
    }
}

void Mixer::renderBlock(float* out, std::size_t frames, double blockStartSeconds) {
    std::fill(out, out + frames * kMixChannels, 0.0f);
    for (Source& source : m_sources) {
        if (source.ringPending && source.gain.current == 0.0) {
            installRing(source, std::move(source.pendingRing), source.pendingClock); // the old clip has faded out
        }
        // A source rises only while it sounds: its fade-in starts with its first sample, not with its request.
        const bool sounding = renderSource(source, frames, blockStartSeconds);
        const bool hold = !sounding && source.gain.target > source.gain.current;
        if (source.gain.current == 0.0) {
            source.pan.current = source.pan.target; // silent: it starts where it is heard, without a sweep
        }
        StereoGains pan = equalPowerGains(source.pan.current);
        for (std::size_t index = 0; index < frames; ++index) {
            const double gain = hold ? source.gain.current : source.gain.advance();
            if (source.pan.current != source.pan.target) {
                pan = equalPowerGains(source.pan.advance());
            }
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
            // std::clamp keeps a NaN: whatever is not a number is silence.
            const double value = static_cast<double>(sample) * gain;
            sample = std::isfinite(value) ? static_cast<float>(std::clamp(value, -1.0, 1.0)) : 0.0f;
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
    summary.p50Milliseconds = errorPercentile(0.50);
    summary.p95Milliseconds = errorPercentile(0.95);
    summary.maxMilliseconds = m_errorMaxMilliseconds;
    return summary;
}

std::uint64_t Mixer::framesRendered() const {
    const std::scoped_lock lock(m_mutex);
    return m_framesRendered;
}

} // namespace rv::audio
