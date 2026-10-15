#include <rv/core/log.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace rv::core {
namespace {

// parseLogLine() splits the lines this pattern writes into their time, level and message.
constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [t%t] %v";
constexpr const char* kLoggerName = "retroverse";

std::mutex g_mutex;
std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_ringSink;
std::filesystem::path g_logFile;
std::atomic<std::uint64_t> g_loggedMessages{0};

/// Counts the messages the logger writes (loggedMessageCount). The count is atomic, so no lock is needed.
class CountingSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& /*message*/) override {
        g_loggedMessages.fetch_add(1, std::memory_order_relaxed);
    }
    void flush_() override {}
};

} // namespace

void initLogging(const LogConfig& config) {
    std::scoped_lock lock(g_mutex);

    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    g_ringSink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(std::max<std::size_t>(config.ringBufferLines, 1));
    sinks.push_back(g_ringSink);
    g_loggedMessages.store(0, std::memory_order_relaxed);
    sinks.push_back(std::make_shared<CountingSink>());

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
    auto console = std::make_shared<spdlog::logger>(kLoggerName, std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    console->set_pattern(kPattern);
    spdlog::set_default_logger(std::move(console));
    g_ringSink.reset();
    g_logFile.clear();
    g_loggedMessages.store(0, std::memory_order_relaxed);
}

std::vector<std::string> recentLogLines(std::size_t maxLines) {
    std::scoped_lock lock(g_mutex);
    if (!g_ringSink) {
        return {};
    }
    return g_ringSink->last_formatted(maxLines);
}

std::uint64_t loggedMessageCount() noexcept {
    return g_loggedMessages.load(std::memory_order_relaxed);
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
