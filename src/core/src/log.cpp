#include <rv/core/log.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace rv::core {
namespace {

// parseLogLine() splits the lines this pattern writes into their time, level and message.
constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [t%t] %v";
constexpr const char* kLoggerName = "retroverse";

/// A copy of the memory sink's count for loggedMessageCount(), which takes no lock.
std::atomic<std::uint64_t> g_loggedMessages{0};
class MemorySink;
std::mutex g_mutex;
std::shared_ptr<MemorySink> g_memorySink;
std::filesystem::path g_logFile;
/// Incremented by every initLogging(): lines and counts of one configuration are never mixed with another's.
std::uint64_t g_generation = 0;
ConsoleStream g_consoleStream = ConsoleStream::Stdout; // of the last configuration, for shutdownLogging()

spdlog::sink_ptr makeConsoleSink(ConsoleStream stream) {
    if (stream == ConsoleStream::Stderr) {
        return std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    }
    return std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
}

/// The in-memory log: the most recent formatted lines and the count of every message, both under the sink's own lock
/// (base_sink's mutex, which the logger takes for each message), so that a reader gets lines and a count that belong
/// together (logLinesSince) even while other threads log.
class MemorySink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit MemorySink(std::size_t capacity) : m_lines(std::max<std::size_t>(capacity, 1)) {}

    /// The newest `maxLines` lines (0: every line kept), oldest first.
    [[nodiscard]] std::vector<std::string> last(std::size_t maxLines) {
        const std::lock_guard lock(mutex_);
        return newest(maxLines == 0 ? m_size : maxLines);
    }

    /// The lines logged after the first `seen` messages (every line kept when `seen` is beyond the count), at most
    /// `maxLines`, with the count they bring the reader to.
    [[nodiscard]] std::pair<std::vector<std::string>, std::uint64_t> since(std::uint64_t seen, std::size_t maxLines) {
        const std::lock_guard lock(mutex_);
        const std::uint64_t added = seen <= m_count ? m_count - seen : m_count;
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(added, maxLines));
        return {newest(wanted), m_count};
    }

    [[nodiscard]] std::uint64_t count() {
        const std::lock_guard lock(mutex_);
        return m_count;
    }

protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(message, formatted);
        std::string* slot = nullptr;
        if (m_size < m_lines.size()) {
            slot = &m_lines[(m_first + m_size) % m_lines.size()];
            ++m_size;
        } else {
            slot = &m_lines[m_first];
            m_first = (m_first + 1) % m_lines.size();
        }
        slot->assign(formatted.data(), formatted.size());
        ++m_count;
        g_loggedMessages.store(m_count, std::memory_order_relaxed);
    }
    void flush_() override {}

private:
    /// The newest `wanted` lines (at most those kept), oldest first; under the lock.
    [[nodiscard]] std::vector<std::string> newest(std::size_t wanted) const {
        const std::size_t taken = std::min(wanted, m_size);
        std::vector<std::string> lines;
        lines.reserve(taken);
        for (std::size_t index = m_size - taken; index < m_size; ++index) {
            lines.push_back(m_lines[(m_first + index) % m_lines.size()]);
        }
        return lines;
    }

    std::vector<std::string> m_lines;
    std::size_t m_first = 0;
    std::size_t m_size = 0;
    std::uint64_t m_count = 0;
};

} // namespace

void initLogging(const LogConfig& config) {
    std::scoped_lock lock(g_mutex);

    std::vector<spdlog::sink_ptr> sinks;
    g_consoleStream = config.consoleStream;
    if (config.console) {
        spdlog::sink_ptr console = makeConsoleSink(config.consoleStream);
        if (config.consoleLevel) {
            console->set_level(*config.consoleLevel);
        }
        sinks.push_back(std::move(console));
    }

    g_memorySink = std::make_shared<MemorySink>(config.ringBufferLines);
    sinks.push_back(g_memorySink);
    g_loggedMessages.store(0, std::memory_order_relaxed);
    ++g_generation;

    g_logFile.clear();
    std::string fileError;
    if (!config.directory.empty()) {
        try {
            std::filesystem::create_directories(config.directory);
            const std::filesystem::path path = config.directory / config.fileName;
            // spdlog is built without wide-char filenames: the path goes through the ANSI code page, which
            // covers %LOCALAPPDATA% on supported systems. Failures are reported and file logging is skipped.
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                path.string(), config.maxFileBytes, std::max<std::size_t>(config.maxFiles, 1)));
            g_logFile = path;
        } catch (const std::exception& e) {
            fileError = e.what();
        }
    }

    auto logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
    logger->set_pattern(kPattern);
    logger->set_level(config.level);
    // Startup, warnings and errors are rare and must survive crashes; per-frame logging is not used.
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));

    if (!fileError.empty()) {
        spdlog::warn("File logging disabled: {}", fileError);
    }
}

void shutdownLogging() {
    std::scoped_lock lock(g_mutex);
    if (auto current = spdlog::default_logger()) {
        current->flush();
    }
    auto console = std::make_shared<spdlog::logger>(kLoggerName, makeConsoleSink(g_consoleStream));
    console->set_pattern(kPattern);
    spdlog::set_default_logger(std::move(console));
    g_memorySink.reset();
    g_logFile.clear();
    g_loggedMessages.store(0, std::memory_order_relaxed);
}

std::vector<std::string> recentLogLines(std::size_t maxLines) {
    std::scoped_lock lock(g_mutex);
    if (!g_memorySink) {
        return {};
    }
    return g_memorySink->last(maxLines);
}

std::uint64_t loggedMessageCount() noexcept {
    return g_loggedMessages.load(std::memory_order_relaxed);
}

LogLinesSince logLinesSince(std::uint64_t generation, std::uint64_t seen, std::size_t maxLines) {
    std::scoped_lock lock(g_mutex);
    LogLinesSince result;
    result.generation = g_generation;
    if (!g_memorySink) {
        return result;
    }
    // Another configuration's count says nothing of this one's lines: every line kept is new.
    auto [lines, count] = g_memorySink->since(generation == g_generation ? seen : ~std::uint64_t{0}, maxLines);
    result.lines = std::move(lines);
    result.count = count;
    return result;
}

std::optional<LogLineFields> parseLogLine(std::string_view line) noexcept {
    // kPattern: "[2026-10-15 06:56:10.734] [warning] [t37204] message\n".
    constexpr std::string_view kSeparator = "] [";
    if (!line.starts_with('[')) {
        return std::nullopt;
    }
    const std::size_t stampEnd = line.find(kSeparator);
    const std::size_t timeStart = line.rfind(' ', stampEnd);
    if (stampEnd == std::string_view::npos || timeStart == std::string_view::npos || timeStart == 0) {
        return std::nullopt;
    }
    const std::size_t levelStart = stampEnd + kSeparator.size();
    const std::size_t levelEnd = line.find(kSeparator, levelStart);
    if (levelEnd == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t threadStart = levelEnd + kSeparator.size();
    const std::size_t threadEnd = line.find("] ", threadStart);
    if (threadEnd == std::string_view::npos || line.substr(threadStart, 1) != "t") {
        return std::nullopt;
    }
    const std::string_view levelName = line.substr(levelStart, levelEnd - levelStart);
    static constexpr std::array<spdlog::level::level_enum, 6> kLevels{spdlog::level::trace, spdlog::level::debug,
                                                                      spdlog::level::info,  spdlog::level::warn,
                                                                      spdlog::level::err,   spdlog::level::critical};
    for (const spdlog::level::level_enum level : kLevels) {
        const spdlog::string_view_t name = spdlog::level::to_string_view(level);
        if (levelName == std::string_view(name.data(), name.size())) {
            std::string_view message = line.substr(threadEnd + 2);
            while (message.ends_with('\n') || message.ends_with('\r')) {
                message.remove_suffix(1);
            }
            return LogLineFields{line.substr(timeStart + 1, stampEnd - timeStart - 1), level, message};
        }
    }
    return std::nullopt;
}

std::filesystem::path activeLogFile() {
    std::scoped_lock lock(g_mutex);
    return g_logFile;
}

} // namespace rv::core
