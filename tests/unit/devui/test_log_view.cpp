#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <charconv>
#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <thread>
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

namespace {

/// The worker and line numbers of "<time>  worker <w> line <n>"; nothing for other text.
std::optional<std::pair<int, int>> workerLine(std::string_view text) {
    const std::size_t at = text.find("worker ");
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    text.remove_prefix(at + 7);
    int worker = 0;
    const auto [afterWorker, workerError] = std::from_chars(text.data(), text.data() + text.size(), worker);
    constexpr std::string_view kLine = " line ";
    const std::string_view rest(afterWorker, static_cast<std::size_t>(text.data() + text.size() - afterWorker));
    if (workerError != std::errc() || !rest.starts_with(kLine)) {
        return std::nullopt;
    }
    int line = 0;
    if (std::from_chars(rest.data() + kLine.size(), rest.data() + rest.size(), line).ec != std::errc()) {
        return std::nullopt;
    }
    return std::pair{worker, line};
}

} // namespace

TEST_CASE("devui/log_view: each line shows once while other threads log (I3 step 13r)", "[devui][log_view]") {
    // Before the fix the view read the count, then the newest lines: a line logged in between took the place of the
    // batch's oldest line and came again with the next batch. Both are read under the in-memory log's lock now, so the
    // view holds every line once and each worker's lines in the order it logged them.
    rv::core::LogConfig config;
    config.console = false;
    config.ringBufferLines = 256;
    rv::core::initLogging(config);

    constexpr int kWorkers = 3;
    constexpr int kLinesPerWorker = 3000;
    std::atomic<int> running{kWorkers};
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([worker, &running, &go] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            for (int line = 0; line < kLinesPerWorker; ++line) {
                spdlog::info("worker {} line {}", worker, line);
            }
            running.fetch_sub(1);
        });
    }
    LogView view(256);
    std::size_t refreshes = 0;
    std::size_t problems = 0;
    const auto check = [&] {
        std::vector<int> last(kWorkers, -1);
        for (std::size_t index = 0; index < view.size(); ++index) {
            const auto parsed = workerLine(view.line(index).text);
            if (!parsed || parsed->first < 0 || parsed->first >= kWorkers) {
                ++problems;
                continue;
            }
            int& previous = last[static_cast<std::size_t>(parsed->first)];
            if (parsed->second <= previous) {
                ++problems; // a line shown twice, or out of order
            }
            previous = parsed->second;
        }
    };
    go.store(true); // the workers log while the view refreshes
    while (running.load() > 0) {
        if (view.refresh()) {
            ++refreshes;
            check();
        }
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    // The last lines (all of them when the workers ended before the first refresh ran).
    if (view.refresh()) {
        ++refreshes;
    }
    check();
    CHECK(problems == 0);
    CHECK(refreshes > 0);
    // Once the log is quiet, the view holds the newest lines the logger keeps, in its order.
    const std::vector<std::string> kept = rv::core::recentLogLines(view.size());
    REQUIRE(kept.size() == view.size());
    for (std::size_t index = 0; index < kept.size(); ++index) {
        const std::string& text = view.line(index).text;
        CHECK(kept[index].find(text.substr(text.find("worker "))) != std::string::npos);
    }
    rv::core::shutdownLogging();
}
