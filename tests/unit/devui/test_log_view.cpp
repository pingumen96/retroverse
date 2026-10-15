#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <rv/core/log.hpp>
#include <rv/devui/log_view.hpp>

using rv::devui::LogView;

TEST_CASE("devui/log_view: lines keep their level and time with the message", "[devui][log_view]") {
    LogView view(8);
    CHECK(view.size() == 0);
    view.append(std::vector<std::string>{"[2026-10-15 06:56:10.734] [warning] [t1] slow frame\r\n",
                                         "[2026-10-15 06:56:10.735] [error] [t1] Shader 'pbr' failed:\nline 2\n",
                                         "not a log line\n"});
    REQUIRE(view.size() == 3);
    // The date and the thread are left out.
    CHECK(view.line(0).level == spdlog::level::warn);
    CHECK(view.line(0).text == "06:56:10.734  slow frame");
    // A message of several lines stays one entry.
    CHECK(view.line(1).level == spdlog::level::err);
    CHECK(view.line(1).text == "06:56:10.735  Shader 'pbr' failed:\nline 2");
    // Text without the log pattern is kept whole, as info.
    CHECK(view.line(2).level == spdlog::level::info);
    CHECK(view.line(2).text == "not a log line");
}

TEST_CASE("devui/log_view: a full view drops its oldest lines", "[devui][log_view]") {
    LogView view(3);
    std::vector<std::string> lines;
    for (int index = 0; index < 5; ++index) {
        lines.push_back("[2026-10-15 06:56:10.734] [info] [t1] line " + std::to_string(index) + "\n");
    }
    view.append(lines);
    REQUIRE(view.size() == 3);
    CHECK(view.line(0).text.ends_with("line 2"));
    CHECK(view.line(2).text.ends_with("line 4"));
    CHECK(LogView(0).capacity() == 1);
}

TEST_CASE("devui/log_view: refresh copies only what the logger wrote since the last refresh", "[devui][log_view]") {
    rv::core::LogConfig config;
    config.console = false;
    config.ringBufferLines = 4;
    rv::core::initLogging(config);

    LogView view(4);
    CHECK_FALSE(view.refresh()); // nothing logged yet
    spdlog::info("one");
    spdlog::warn("two");
    CHECK(view.refresh());
    REQUIRE(view.size() == 2);
    CHECK(view.line(0).text.ends_with("  one"));
    CHECK(view.line(1).level == spdlog::level::warn);
    CHECK_FALSE(view.refresh()); // quiet log: nothing to copy

    // More lines than the view keeps: the newest ones.
    for (int index = 0; index < 6; ++index) {
        spdlog::error("burst {}", index);
    }
    CHECK(view.refresh());
    REQUIRE(view.size() == 4);
    CHECK(view.line(0).text.ends_with("burst 2"));
    CHECK(view.line(3).text.ends_with("burst 5"));

    // Logging configured again: the view starts over with the new log.
    rv::core::initLogging(config);
    spdlog::critical("fresh");
    CHECK(view.refresh());
    REQUIRE(view.size() == 1);
    CHECK(view.line(0).level == spdlog::level::critical);
    rv::core::shutdownLogging();
}
