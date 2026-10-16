#include <rv/media/presentation_clock.hpp>

#include <algorithm>

namespace rv::media {

void PresentationClock::skipStaleFrames(FrameSource& frames) {
    for (;;) {
        const std::optional<FrameTiming> front = frames.peek(0);
        if (!front) {
            return;
        }
        if (front->generation < m_generation) {
            frames.pop(false);
            ++m_stats.stale;
            continue;
        }
        if (front->generation > m_generation) {
            // A seek: the clock starts again at the first frame after it.
            m_generation = front->generation;
            restart();
        }
        return;
    }
}

ClockUpdate PresentationClock::update(double nowSeconds, FrameSource& frames, bool sourceEnded) {
    ClockUpdate result;
    ++m_stats.updates;
    skipStaleFrames(frames);
    if (m_paused) {
        result.shown = m_current;
        if (m_anchor) {
            result.mediaTime = m_pausedMediaTime;
        }
        return result;
    }
    if (!m_anchor) {
        // The media time starts at the first frame shown, when it is shown.
        const std::optional<FrameTiming> first = frames.peek(0);
        if (!first) {
            return result;
        }
        m_anchor = Anchor{nowSeconds, first->ptsSeconds};
        m_previousMediaTime.reset();
    }
    double media = m_anchor->mediaTime + (nowSeconds - m_anchor->renderTime);

    // The frames at the front whose time has come; only the newest of them is shown.
    std::size_t due = 0;
    for (;;) {
        const std::optional<FrameTiming> timing = frames.peek(due);
        if (!timing || timing->generation != m_generation ||
            timing->ptsSeconds > media + m_settings.dueToleranceSeconds) {
            break;
        }
        ++due;
    }

    if (due > 0) {
        for (std::size_t skipped = 0; skipped + 1 < due; ++skipped) {
            frames.pop(false);
            ++m_stats.dropped;
            ++result.dropped;
        }
        const FrameTiming shown = *frames.peek(0);
        frames.pop(true);
        // Late: it was already due at the previous update but was not there yet.
        result.late = m_previousMediaTime.has_value() &&
                      shown.ptsSeconds <= *m_previousMediaTime + m_settings.dueToleranceSeconds;
        const double lateness = std::max(0.0, media - shown.ptsSeconds);
        if (lateness > m_settings.resyncThresholdSeconds) {
            // Too far behind to catch up by skipping: the media time moves back to this frame.
            m_anchor = Anchor{nowSeconds, shown.ptsSeconds};
            m_stats.resyncSeconds += lateness;
            ++m_stats.resyncs;
            result.resynced = true;
            media = shown.ptsSeconds;
        }
        m_current = shown;
        result.newFrame = true;
        ++m_stats.presented;
        if (result.late) {
            ++m_stats.late;
        }
        m_stats.totalLatenessSeconds += lateness;
        m_stats.maxLatenessSeconds = std::max(m_stats.maxLatenessSeconds, lateness);
    } else if (m_current && !sourceEnded &&
               media > m_current->ptsSeconds + m_current->durationSeconds + m_settings.dueToleranceSeconds) {
        // The frame on screen has had its time and nothing newer is due: the decoder is behind.
        result.repeated = true;
        ++m_stats.repeated;
    }
    m_previousMediaTime = media;
    result.mediaTime = media;
    result.shown = m_current;
    return result;
}

void PresentationClock::pause(double nowSeconds) noexcept {
    if (m_paused) {
        return;
    }
    m_paused = true;
    if (m_anchor) {
        m_pausedMediaTime = m_anchor->mediaTime + (nowSeconds - m_anchor->renderTime);
    }
}

void PresentationClock::resume(double nowSeconds) noexcept {
    if (!m_paused) {
        return;
    }
    m_paused = false;
    if (m_anchor) {
        m_anchor = Anchor{nowSeconds, m_pausedMediaTime};
    }
}

void PresentationClock::restart() noexcept {
    m_anchor.reset();
    m_current.reset();
    m_previousMediaTime.reset();
}

std::optional<double> PresentationClock::mediaTime(double nowSeconds) const noexcept {
    if (!m_anchor) {
        return std::nullopt;
    }
    if (m_paused) {
        return m_pausedMediaTime;
    }
    return m_anchor->mediaTime + (nowSeconds - m_anchor->renderTime);
}

} // namespace rv::media
