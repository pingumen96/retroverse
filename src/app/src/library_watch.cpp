#include <rv/app/library_watch.hpp>

#include <algorithm>

namespace rv::app {

void LibraryWatch::change(Clock::time_point now, const std::optional<std::string>& system) {
    if (m_changes == 0) {
        m_firstChange = now;
        m_windowStart = now;
    }
    ++m_changes;
    m_lastChange = now;
    if (m_heldNow) {
        ++m_heldChanges;
    }
    if (system) {
        m_systems.insert(*system);
    } else {
        m_everything = true;
    }
}

std::optional<WatchRefresh> LibraryWatch::due(Clock::time_point now, bool held) {
    if (m_changes == 0) {
        m_heldNow = held;
        return std::nullopt;
    }
    if (held) {
        if (!m_heldNow) {
            m_heldChanges += m_changes; // those waiting when the hold began
        }
        m_heldNow = true;
        m_heldSome = true;
        return std::nullopt;
    }
    if (m_heldNow) {
        // The hold ended: the window opens now, as if the changes had come at the return.
        m_heldNow = false;
        m_windowStart = now;
        m_lastChange = now;
    }
    const Clock::time_point quietEnd = *m_lastChange + m_timings.quiet;
    const Clock::time_point longestEnd = *m_windowStart + m_timings.longest;
    bool burst = false;
    if (now >= quietEnd) {
        burst = false;
    } else if (now >= longestEnd) {
        if (m_lastBurst && m_lastRefresh && now < *m_lastRefresh + m_timings.burstInterval) {
            return std::nullopt; // the burst goes on: one refresh a minute
        }
        burst = true;
    } else {
        return std::nullopt;
    }
    WatchRefresh refresh;
    refresh.everything = m_everything;
    if (!m_everything) {
        refresh.systems.assign(m_systems.begin(), m_systems.end());
    }
    refresh.changes = m_changes;
    refresh.firstChange = *m_firstChange;
    refresh.held = m_heldSome;
    refresh.burst = burst;
    m_dueBurst = burst;
    return refresh;
}

void LibraryWatch::started(Clock::time_point now) {
    m_lastRefresh = now;
    m_lastBurst = m_dueBurst;
    m_dueBurst = false;
    m_systems.clear();
    m_everything = false;
    m_changes = 0;
    m_firstChange.reset();
    m_windowStart.reset();
    m_lastChange.reset();
    m_heldSome = false;
}

std::optional<LibraryWatch::Clock::time_point> LibraryWatch::nextDue() const {
    if (m_changes == 0 || m_heldNow) {
        return std::nullopt;
    }
    const Clock::time_point quietEnd = *m_lastChange + m_timings.quiet;
    Clock::time_point longestEnd = *m_windowStart + m_timings.longest;
    if (m_lastBurst && m_lastRefresh) {
        longestEnd = std::max(longestEnd, *m_lastRefresh + m_timings.burstInterval);
    }
    return std::min(quietEnd, longestEnd);
}

} // namespace rv::app
