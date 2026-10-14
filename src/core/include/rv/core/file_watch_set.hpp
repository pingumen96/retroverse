#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace rv::core {

/// Detects modifications of a set of files by polling their last write times, at most once per interval (shader hot
/// reload). The state of a file is its last write time, or "missing" when it cannot be queried; any difference from
/// the recorded state is a change, so files that appear or disappear are reported too. Paths are compared after
/// lexical normalization (no file system access; letter case is preserved). Not thread-safe: poll from one thread.
class FileWatchSet {
public:
    using Clock = std::chrono::steady_clock;
    /// Returns the current time. Injectable so tests control the throttling; empty means Clock::now.
    using TimeSource = std::function<Clock::time_point()>;

    /// Default minimum time between two file system checks.
    static constexpr std::chrono::milliseconds kDefaultInterval{500};

    /// A negative interval counts as zero (every call to changed() checks). The first check happens one interval
    /// after construction.
    explicit FileWatchSet(std::chrono::milliseconds interval = kDefaultInterval, TimeSource now = {});

    /// Watches `file`: its current state becomes the reference. Adding a file that is already watched keeps its
    /// reference, so a change that happened in between is still reported.
    void add(const std::filesystem::path& file);
    /// Watches exactly `files`: files already watched keep their reference, new ones start from their current
    /// state, the others are dropped.
    void assign(std::span<const std::filesystem::path> files);
    /// Stops watching `file`; no-op when it is not watched.
    void remove(const std::filesystem::path& file);
    /// Stops watching every file. The throttling clock is unaffected.
    void clear() noexcept;

    [[nodiscard]] bool contains(const std::filesystem::path& file) const;
    [[nodiscard]] std::size_t size() const noexcept { return m_files.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_files.empty(); }

    /// When at least the interval has elapsed since the previous check, queries every watched file and returns the
    /// ones whose state differs from the reference (normalized paths, sorted), making their new state the reference
    /// so that each modification is reported once. Otherwise returns an empty list without touching the file system.
    /// A time source that goes backwards restarts the interval.
    [[nodiscard]] std::vector<std::filesystem::path> changed();

private:
    /// Last write time, or nothing for a file that cannot be queried (missing, inaccessible).
    using FileState = std::optional<std::filesystem::file_time_type>;

    std::chrono::milliseconds m_interval;
    TimeSource m_now;
    Clock::time_point m_lastCheck;
    std::map<std::filesystem::path, FileState> m_files; // key: lexically normal path
};

} // namespace rv::core
