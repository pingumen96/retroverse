#pragma once

// A core::IChangeWatcher for tests (ADR-0018 §10): it watches nothing; the test says what changed (push()) and how
// each root stands (setMode()), and reads what was asked of it (roots(), watched()).

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/core/change_watcher.hpp>

namespace rv::test {

class FakeChangeWatcher final : public core::IChangeWatcher {
public:
    void watch(std::vector<core::WatchRoot> roots) override {
        const std::lock_guard lock(m_mutex);
        m_status.assign(roots.size(), core::RootStatus{});
        for (std::size_t index = 0; index < roots.size(); ++index) {
            m_status[index].name = roots[index].name;
            m_status[index].mode = core::WatchMode::Notify;
        }
        m_roots = std::move(roots);
        m_changes.clear();
        ++m_watched;
    }
    [[nodiscard]] std::vector<core::Change> take() override {
        const std::lock_guard lock(m_mutex);
        return std::exchange(m_changes, {});
    }
    [[nodiscard]] std::vector<core::RootStatus> status() const override {
        const std::lock_guard lock(m_mutex);
        return m_status;
    }

    /// A change of `path` below the root named `root` (the first root when none has that name).
    void push(std::string_view root, core::ChangeKind kind, std::filesystem::path path) {
        const std::lock_guard lock(m_mutex);
        m_changes.push_back(core::Change{indexOf(root), kind, std::move(path)});
    }
    void setMode(std::string_view root, core::WatchMode mode) {
        const std::lock_guard lock(m_mutex);
        if (const std::size_t index = indexOf(root); index < m_status.size()) {
            m_status[index].mode = mode;
        }
    }
    [[nodiscard]] std::vector<core::WatchRoot> roots() const {
        const std::lock_guard lock(m_mutex);
        return m_roots;
    }
    /// watch() calls so far.
    [[nodiscard]] std::size_t watched() const {
        const std::lock_guard lock(m_mutex);
        return m_watched;
    }

private:
    [[nodiscard]] std::size_t indexOf(std::string_view root) const {
        for (std::size_t index = 0; index < m_roots.size(); ++index) {
            if (m_roots[index].name == root) {
                return index;
            }
        }
        return 0;
    }

    mutable std::mutex m_mutex;
    std::vector<core::WatchRoot> m_roots;
    std::vector<core::RootStatus> m_status;
    std::vector<core::Change> m_changes;
    std::size_t m_watched = 0;
};

} // namespace rv::test
