#include <rv/app/library_watcher.hpp>

#include <utility>

#include <spdlog/spdlog.h>

#include <rv/core/utf8.hpp>

namespace rv::app {

void LibraryWatcher::arm(esde::ChangeScopeMap map) {
    std::vector<core::WatchRoot> roots = esde::watchRoots(map);
    m_index.emplace(std::move(map));
    if (m_arms > 0 && roots == m_roots) {
        return;
    }
    m_roots = roots;
    ++m_arms;
    spdlog::info("Library watcher: {} root(s) watched", m_roots.size());
    m_watcher.watch(std::move(roots));
}

void LibraryWatcher::take(Clock::time_point now) {
    for (const core::Change& change : m_watcher.take()) {
        ++m_changes;
        switch (change.kind) {
        case core::ChangeKind::Overflow:
            ++m_overflows;
            ++m_mapped;
            m_policy.change(now, std::nullopt);
            continue;
        case core::ChangeKind::RootLost:
        case core::ChangeKind::RootBack:
            ++(change.kind == core::ChangeKind::RootLost ? m_rootsLost : m_rootsBack);
            ++m_mapped;
            m_policy.change(now, std::nullopt);
            continue;
        default:
            break;
        }
        if (!m_index) {
            ++m_ignored;
            ++m_ignoredBy["no-map"];
            continue;
        }
        // A folder's own time (Windows reports it when anything inside changes: an emulator's save beside a ROM): what
        // matters inside comes with its own name. The fallback poll's folder times are Polled, and count.
        if (change.kind == core::ChangeKind::Modified && change.path.extension().empty()) {
            ++m_ignored;
            ++m_ignoredBy["folder-time"];
            continue;
        }
        const esde::ChangeScope scope = m_index->scope(change.path, change.directory);
        // The path may name the user's folders: at debug level only.
        spdlog::debug("Library watcher: {} {}: {}{} ({})", core::toString(change.kind),
                      core::pathToUtf8Lossy(change.path), esde::toString(scope.kind),
                      scope.kind == esde::ScopeKind::System ? " " + scope.system.str() : std::string(), scope.reason);
        switch (scope.kind) {
        case esde::ScopeKind::None:
            ++m_ignored;
            ++m_ignoredBy[std::string(scope.reason)];
            break;
        case esde::ScopeKind::System:
            ++m_mapped;
            m_policy.change(now, scope.system.str());
            break;
        case esde::ScopeKind::Everything:
            ++m_mapped;
            m_policy.change(now, std::nullopt);
            break;
        }
    }
}

std::optional<WatchRefresh> LibraryWatcher::poll(Clock::time_point now, bool held) {
    take(now);
    return m_policy.due(now, held);
}

void LibraryWatcher::started(Clock::time_point now) {
    ++m_refreshes;
    m_refusing = false;
    m_policy.started(now);
}

void LibraryWatcher::refused() {
    if (!std::exchange(m_refusing, true)) {
        ++m_refused;
    }
}

RefreshRequest LibraryWatcher::requestOf(const WatchRefresh& refresh) {
    RefreshRequest request;
    request.trigger = RefreshTrigger::Watcher;
    request.acceptRemovals = false; // FR-033: a watcher's refresh never accepts removals
    if (!refresh.everything) {
        for (const std::string& system : refresh.systems) {
            if (auto name = catalog::SystemName::parse(system)) {
                request.systems.push_back(std::move(*name));
            } else {
                return RefreshRequest{RefreshTrigger::Watcher, {}, false}; // not a name: every system
            }
        }
    }
    return request;
}

void LibraryWatcher::record(WatcherRecord& record) const {
    record.enabled = m_arms > 0;
    record.roots.clear();
    for (const core::RootStatus& status : m_watcher.status()) {
        WatchRootRecord root;
        root.name = status.name;
        root.mode = std::string(core::toString(status.mode));
        root.lost = status.lost;
        root.back = status.back;
        root.reason = status.reason;
        record.roots.push_back(std::move(root));
    }
    record.changes = m_changes;
    record.mapped = m_mapped;
    record.ignored = m_ignored;
    record.ignoredBy.clear();
    for (const auto& [reason, count] : m_ignoredBy) {
        record.ignoredBy.push_back(WatchReasonCount{reason, count});
    }
    record.overflows = m_overflows;
    record.rootsLost = m_rootsLost;
    record.rootsBack = m_rootsBack;
    record.held = m_policy.heldChanges();
    record.refreshes = m_refreshes;
    record.refused = m_refused;
}

} // namespace rv::app
