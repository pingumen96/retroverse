#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <rv/core/log.hpp>

using rv::core::LogLineFields;
using rv::core::parseLogLine;

namespace {

/// Logging into the in-memory lines only (no console, no file), restored to the plain console logger afterwards.
class MemoryLogging {
public:
    explicit MemoryLogging(std::size_t ringLines) {
        rv::core::LogConfig config;
        config.console = false;
        config.ringBufferLines = ringLines;
        rv::core::initLogging(config);
    }
    ~MemoryLogging() { rv::core::shutdownLogging(); }
    MemoryLogging(const MemoryLogging&) = delete;
    MemoryLogging& operator=(const MemoryLogging&) = delete;
};

std::optional<spdlog::level::level_enum> levelOf(const std::string& line) {
    const std::optional<LogLineFields> fields = parseLogLine(line);
    return fields ? std::optional(fields->level) : std::nullopt;
}

} // namespace

TEST_CASE("core/log: log lines split into time, level and message", "[core][log]") {
    const std::optional<LogLineFields> fields =
        parseLogLine("[2026-10-15 06:56:10.734] [warning] [t37204] Scene [m0] loaded\r\n");
    REQUIRE(fields.has_value());
    CHECK(fields->time == "06:56:10.734");
    CHECK(fields->level == spdlog::level::warn);
    CHECK(fields->message == "Scene [m0] loaded");

    // A message of several lines keeps its inner line breaks; an empty one is empty.
    const auto multiline = parseLogLine("[2026-10-15 06:56:10.734] [error] [t1] Shader failed:\nline 2\n");
    REQUIRE(multiline.has_value());
    CHECK(multiline->message == "Shader failed:\nline 2");
    const auto empty = parseLogLine("[2026-10-15 06:56:10.734] [info] [t1] \n");
    REQUIRE(empty.has_value());
    CHECK(empty->message.empty());

    CHECK(levelOf("[2026-10-15 06:56:10.734] [trace] [t1] a") == spdlog::level::trace);
    CHECK(levelOf("[2026-10-15 06:56:10.734] [debug] [t1] a") == spdlog::level::debug);
    CHECK(levelOf("[2026-10-15 06:56:10.734] [info] [t1] a") == spdlog::level::info);
    CHECK(levelOf("[2026-10-15 06:56:10.734] [error] [t1] a") == spdlog::level::err);
    CHECK(levelOf("[2026-10-15 06:56:10.734] [critical] [t1] a") == spdlog::level::critical);

    // Text that does not follow the pattern has no fields.
    CHECK_FALSE(parseLogLine("").has_value());
    CHECK_FALSE(parseLogLine("info: plain text").has_value());
    CHECK_FALSE(parseLogLine("[2026-10-15 06:56:10.734]").has_value());
    CHECK_FALSE(parseLogLine("[2026-10-15 06:56:10.734] [info").has_value());
    CHECK_FALSE(parseLogLine("[2026-10-15 06:56:10.734] [info] message without thread").has_value());
    CHECK_FALSE(parseLogLine("[2026-10-15 06:56:10.734] [info] [x1] a").has_value());
    CHECK_FALSE(parseLogLine("[06:56:10.734] [info] [t1] a").has_value()); // no date
    CHECK_FALSE(parseLogLine("[2026-10-15 06:56:10.734] [verbose] [t1] a").has_value());
    CHECK_FALSE(parseLogLine("[2026-10-15 06:56:10.734] [off] [t1] a").has_value());
}

TEST_CASE("core/log: the in-memory lines keep the most recent messages and count every one", "[core][log]") {
    const MemoryLogging logging(3);
    CHECK(rv::core::loggedMessageCount() == 0);
    CHECK(rv::core::recentLogLines().empty());

    spdlog::info("first");
    spdlog::warn("second");
    spdlog::debug("below the logger level: neither kept nor counted");
    spdlog::error("third");
    spdlog::critical("fourth");
    CHECK(rv::core::loggedMessageCount() == 4);

    // Only the last three are kept, oldest first; each parses back to its level and message.
    const std::vector<std::string> lines = rv::core::recentLogLines();
    REQUIRE(lines.size() == 3);
    const std::optional<LogLineFields> second = parseLogLine(lines[0]);
    REQUIRE(second.has_value());
    CHECK(second->level == spdlog::level::warn);
    CHECK(second->message == "second");
    CHECK(second->time.size() == 12); // HH:MM:SS.mmm
    CHECK(levelOf(lines[1]) == spdlog::level::err);
    CHECK(levelOf(lines[2]) == spdlog::level::critical);
    // A limit returns the newest lines.
    const std::vector<std::string> newest = rv::core::recentLogLines(1);
    REQUIRE(newest.size() == 1);
    CHECK(newest[0] == lines[2]);

    rv::core::shutdownLogging();
    CHECK(rv::core::loggedMessageCount() == 0);
    CHECK(rv::core::recentLogLines().empty());
}
