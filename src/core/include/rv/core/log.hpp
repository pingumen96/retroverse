#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

/// Most recent formatted log lines, oldest first, at most `maxLines` (0 returns every line kept). Each line reads
/// "[<date> <time>] [<level>] [t<thread>] <message>" and ends with the platform line break.
[[nodiscard]] std::vector<std::string> recentLogLines(std::size_t maxLines = 512);

/// Messages the logger has written since the last initLogging(), also those the in-memory lines no longer hold; 0
/// after shutdownLogging(). Readers of recentLogLines() poll it to fetch only what is new.
[[nodiscard]] std::uint64_t loggedMessageCount() noexcept;

/// The fields of a line returned by recentLogLines(), as views into that line.
struct LogLineFields {
    std::string_view time; ///< "HH:MM:SS.mmm"
    spdlog::level::level_enum level = spdlog::level::info;
    std::string_view message; ///< what was logged, without the line break (it may span several lines)
};

/// Splits a line returned by recentLogLines(), "[<date> <time>] [<level>] [t<thread>] <message>". Nothing for text that
/// does not follow that pattern.
[[nodiscard]] std::optional<LogLineFields> parseLogLine(std::string_view line) noexcept;

/// Path of the active log file, or empty when file logging is disabled.
[[nodiscard]] std::filesystem::path activeLogFile();

} // namespace rv::core
