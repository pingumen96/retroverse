#include <rv/core/file_watch_set.hpp>

#include <algorithm>
#include <set>
#include <utility>

#include <rv/core/file_io.hpp>

namespace rv::core {

FileWatchSet::FileWatchSet(std::chrono::milliseconds interval, TimeSource now)
    : m_interval(std::max(interval, std::chrono::milliseconds::zero())),
      m_now(now ? std::move(now) : TimeSource([] { return Clock::now(); })), m_lastCheck(m_now()) {}

void FileWatchSet::add(const std::filesystem::path& file) {
    std::filesystem::path key = file.lexically_normal();
    if (!m_files.contains(key)) {
        FileState state = lastWriteTime(key);
        m_files.emplace(std::move(key), state);
    }
}

void FileWatchSet::assign(std::span<const std::filesystem::path> files) {
    std::set<std::filesystem::path> wanted;
    for (const std::filesystem::path& file : files) {
        wanted.insert(file.lexically_normal());
    }
    std::erase_if(m_files, [&wanted](const auto& entry) { return !wanted.contains(entry.first); });
    for (const std::filesystem::path& file : wanted) {
        add(file);
    }
}

void FileWatchSet::remove(const std::filesystem::path& file) {
    m_files.erase(file.lexically_normal());
}

void FileWatchSet::clear() noexcept {
    m_files.clear();
}

bool FileWatchSet::contains(const std::filesystem::path& file) const {
    return m_files.contains(file.lexically_normal());
}

std::vector<std::filesystem::path> FileWatchSet::changed() {
    const Clock::time_point now = m_now();
    if (now < m_lastCheck) {
        m_lastCheck = now;
        return {};
    }
    if (now - m_lastCheck < m_interval) {
        return {};
    }
    m_lastCheck = now;

    std::vector<std::filesystem::path> result;
    for (auto& [file, state] : m_files) { // std::map: sorted
        const FileState current = lastWriteTime(file);
        if (current != state) {
            state = current;
            result.push_back(file);
        }
    }
    return result;
}

} // namespace rv::core
