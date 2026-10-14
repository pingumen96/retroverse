#include <rv/core/log.hpp>

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace rv::core {
namespace {

constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [t%t] %v";
constexpr const char* kLoggerName = "retroverse";

std::mutex g_mutex;
std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_ringSink;
std::filesystem::path g_logFile;

} // namespace

void initLogging(const LogConfig& config) {
    std::scoped_lock lock(g_mutex);

    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    g_ringSink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(std::max<std::size_t>(config.ringBufferLines, 1));
    sinks.push_back(g_ringSink);

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
}

std::vector<std::string> recentLogLines(std::size_t maxLines) {
    std::scoped_lock lock(g_mutex);
    if (!g_ringSink) {
        return {};
    }
    return g_ringSink->last_formatted(maxLines);
}

std::filesystem::path activeLogFile() {
    std::scoped_lock lock(g_mutex);
    return g_logFile;
}

} // namespace rv::core
