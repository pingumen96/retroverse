#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace rv::core {

/// Logging configuration. Modules log through spdlog's default logger: `spdlog::info("...")`.
struct LogConfig {
    /// Directory of the rotating log file; empty disables file logging.
    std::filesystem::path directory;
    std::string fileName = "retroverse.log";
    bool console = true;
    spdlog::level::level_enum level = spdlog::level::info;
    std::size_t maxFileBytes = 5u * 1024u * 1024u;
    std::size_t maxFiles = 3;
    /// Recent formatted lines kept in memory for the developer overlay.
    std::size_t ringBufferLines = 512;
};

/// Installs the "retroverse" logger (console + rotating file + in-memory ring buffer) as spdlog's default
/// logger. Safe to call more than once: the previous configuration is replaced. Never throws: when the
/// file sink cannot be created, logging continues on the remaining sinks and a warning is emitted.
void initLogging(const LogConfig& config);

/// Flushes every sink and reverts to a plain console logger (logging stays valid after this call).
void shutdownLogging();

/// Most recent formatted log lines, oldest first, at most `maxLines`.
[[nodiscard]] std::vector<std::string> recentLogLines(std::size_t maxLines = 512);

/// Path of the active log file, or empty when file logging is disabled.
[[nodiscard]] std::filesystem::path activeLogFile();

} // namespace rv::core
