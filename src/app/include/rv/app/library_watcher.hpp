#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The file watcher of retroverse.exe (ADR-0018 §10), between the watcher port and the catalog: it watches the roots of
// the library's map (esde::watchRoots(), from the catalog worker's CatalogResults::watchMap), maps each change the
// watcher brings (esde::changeScope(): a system, every system, nothing; an overflow, a root lost or back: every system)
// and hands the policy (LibraryWatch) those that matter. poll() says when a refresh is due and of what: the caller asks
// the catalog for it (RefreshRequest with the trigger `watcher`, never accepting removals) and says whether it started.
// Main thread only; the watcher's own thread is the adapter's (platform_win::DirectoryWatcher).

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/library_refresh.hpp>
#include <rv/app/library_watch.hpp>
#include <rv/app/run_report.hpp>
#include <rv/core/change_watcher.hpp>
#include <rv/esde/change_scope.hpp>

namespace rv::app {

class LibraryWatcher {
public:
    using Clock = std::chrono::steady_clock;

    explicit LibraryWatcher(core::IChangeWatcher& watcher, WatchTimings timings = {}) noexcept
        : m_watcher(watcher), m_policy(timings) {}

    /// The library's map from now on: its roots watched (asked of the watcher again only when they differ).
    void arm(esde::ChangeScopeMap map);
    /// The watcher's changes since the last look, mapped and given to the policy at `now`; the refresh due now, if any
    /// (`held`: a game runs, the editor is open, a ride: nothing is due).
    [[nodiscard]] std::optional<WatchRefresh> poll(Clock::time_point now, bool held);
    /// The refresh poll() gave started at `now` (its changes answered), or the catalog could not take it now (asked again
    /// at a later poll()).
    void started(Clock::time_point now);
    void refused();

    /// What the catalog is asked for `refresh`.
    [[nodiscard]] static RefreshRequest requestOf(const WatchRefresh& refresh);
    /// The map armed, if any.
    [[nodiscard]] const esde::ChangeScopeMap* map() const noexcept { return m_index ? &m_index->map() : nullptr; }
    [[nodiscard]] const LibraryWatch& policy() const noexcept { return m_policy; }

    /// The run report's `watcher`: the roots and their modes, the changes seen and what they meant.
    void record(WatcherRecord& record) const;
    /// I7 step 8: whether it watches (a map was armed), and each root's mode by its name (the library screen's).
    [[nodiscard]] bool armed() const noexcept { return m_arms > 0; }
    [[nodiscard]] std::vector<core::RootStatus> status() const { return m_watcher.status(); }

private:
    void take(Clock::time_point now);

    core::IChangeWatcher& m_watcher;
    LibraryWatch m_policy;
    std::optional<esde::ChangeScopeIndex> m_index;
    std::vector<core::WatchRoot> m_roots;
    std::uint64_t m_arms = 0;
    std::uint64_t m_changes = 0;
    std::uint64_t m_mapped = 0;
    std::uint64_t m_ignored = 0;
    std::uint64_t m_overflows = 0;
    std::uint64_t m_rootsLost = 0;
    std::uint64_t m_rootsBack = 0;
    std::uint64_t m_refreshes = 0;
    std::uint64_t m_refused = 0;
    /// The last poll's refresh was refused: one refusal is counted until one starts.
    bool m_refusing = false;
    std::map<std::string, std::uint64_t> m_ignoredBy;
};

} // namespace rv::app
