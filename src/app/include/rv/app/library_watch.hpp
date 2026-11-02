#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or RmlUi header is included.
//
// When the file watcher's changes start a refresh (ADR-0018 §10), on an injected clock, the changes already mapped to a
// system or to every system (esde::changeScope(), LibraryWatcher's):
//  - a change opens a window, which ends after WatchTimings::quiet (2 s) without another, or WatchTimings::longest (30 s)
//    after its first change while changes keep coming (ES-DE scraping thousands of files);
//  - a window that reached its longest after one that did too (a burst that goes on) waits until burstInterval (60 s)
//    passed since the refresh before it: at most one refresh a minute while the burst lasts; the window that ends in
//    quiet, the burst's last, goes at once;
//  - while held (a game runs, the editor is open, a ride) nothing starts: the changes are kept, and the window opens
//    again when the hold ends, as if they had come then;
//  - the refresh covers the systems its changes named, or every system once one change named every system (a buffer
//    overflow, a change no system owns that may matter, a root lost or back, the configuration).
// A refresh the catalog cannot take now (one runs) is asked again at the next look: the changes stay until started().

#include <chrono>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rv::app {

struct WatchTimings {
    std::chrono::milliseconds quiet{2000};
    std::chrono::milliseconds longest{30000};
    std::chrono::milliseconds burstInterval{60000};
};

/// A refresh LibraryWatch asks for.
struct WatchRefresh {
    /// The systems, by name (empty with `everything`).
    std::vector<std::string> systems;
    bool everything = false;
    /// The changes it answers, and when the first of them came.
    std::size_t changes = 0;
    std::chrono::steady_clock::time_point firstChange{};
    /// Some of them waited for a hold to end.
    bool held = false;
    /// Its window ended at WatchTimings::longest, changes still coming.
    bool burst = false;

    bool operator==(const WatchRefresh&) const = default;
};

class LibraryWatch {
public:
    using Clock = std::chrono::steady_clock;

    explicit LibraryWatch(WatchTimings timings = {}) noexcept : m_timings(timings) {}

    /// A change at `now` of `system`, or of every system (nothing).
    void change(Clock::time_point now, const std::optional<std::string>& system);
    /// The refresh to start at `now`, if one is due; `held`: none starts (see above).
    [[nodiscard]] std::optional<WatchRefresh> due(Clock::time_point now, bool held);
    /// The refresh due() gave started at `now`: the changes it answers are forgotten.
    void started(Clock::time_point now);

    /// Changes wait for a refresh.
    [[nodiscard]] bool pending() const noexcept { return m_changes > 0; }
    /// Changes that came, or were waiting, while held, in all.
    [[nodiscard]] std::size_t heldChanges() const noexcept { return m_heldChanges; }

private:
    WatchTimings m_timings;
    std::set<std::string> m_systems;
    bool m_everything = false;
    std::size_t m_changes = 0;
    std::optional<Clock::time_point> m_firstChange;
    std::optional<Clock::time_point> m_windowStart;
    std::optional<Clock::time_point> m_lastChange;
    /// The changes waiting were held at the last look: the window opens again when the hold ends.
    bool m_heldNow = false;
    bool m_heldSome = false;
    std::size_t m_heldChanges = 0;
    std::optional<Clock::time_point> m_lastRefresh;
    /// The last refresh's window ended at its longest: the next one that does waits for burstInterval.
    bool m_lastBurst = false;
    /// What the last due() gave: a burst window.
    bool m_dueBurst = false;
};

} // namespace rv::app
