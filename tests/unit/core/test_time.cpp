#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include <rv/core/time.hpp>

using namespace std::chrono_literals;
using rv::core::formatUtcIso8601;
using rv::core::parseUtcIso8601;
using std::chrono::system_clock;

namespace {

system_clock::time_point at(std::chrono::sys_days day, system_clock::duration since) {
    return system_clock::time_point(day) + since;
}

} // namespace

TEST_CASE("core/time: UTC times as ISO-8601", "[core][time]") {
    using std::chrono::year;
    CHECK(formatUtcIso8601(system_clock::time_point{}) == "1970-01-01T00:00:00.000Z");
    CHECK(formatUtcIso8601(at(year{2026} / 10 / 16, 10h + 15min + 30s + 123ms)) == "2026-10-16T10:15:30.123Z");
    // Milliseconds rounded down, also before 1970.
    CHECK(formatUtcIso8601(at(year{2026} / 1 / 2, 999'999us)) == "2026-01-02T00:00:00.999Z");
    CHECK(formatUtcIso8601(system_clock::time_point{} - 1ms) == "1969-12-31T23:59:59.999Z");
    CHECK(formatUtcIso8601(system_clock::time_point{} - 1us) == "1969-12-31T23:59:59.999Z");
    // The bogus file times of the ROM tree: the FAT epoch and the 32-bit limit.
    CHECK(formatUtcIso8601(at(year{1980} / 1 / 1, 0s)) == "1980-01-01T00:00:00.000Z");
    CHECK(formatUtcIso8601(at(year{2038} / 1 / 19, 3h + 14min + 7s)) == "2038-01-19T03:14:07.000Z");
    CHECK(formatUtcIso8601(at(year{2024} / 2 / 29, 23h + 59min + 59s)) == "2024-02-29T23:59:59.000Z");
    // Fixed width: the text sorts like the times.
    CHECK(formatUtcIso8601(at(year{1} / 1 / 1, 0s)) == "0001-01-01T00:00:00.000Z");
    CHECK(formatUtcIso8601(at(year{9999} / 12 / 31, 0s)).size() == 24);
}

TEST_CASE("core/time: utcNow", "[core][time]") {
    const auto before = std::chrono::floor<std::chrono::milliseconds>(system_clock::now());
    const std::string now = rv::core::utcNow();
    const auto after = system_clock::now();
    REQUIRE(now.size() == 24);
    CHECK(now[10] == 'T');
    CHECK(now.back() == 'Z');
    const auto parsed = parseUtcIso8601(now);
    REQUIRE(parsed.has_value());
    CHECK(*parsed >= before);
    CHECK(*parsed <= after);
}

TEST_CASE("core/time: reading ISO-8601 back", "[core][time]") {
    using std::chrono::year;
    const auto time = at(year{2026} / 10 / 16, 10h + 15min + 30s + 123ms);
    CHECK(parseUtcIso8601(formatUtcIso8601(time)).value() == time);
    CHECK(parseUtcIso8601("2001-02-03T04:05:06Z").value() == at(year{2001} / 2 / 3, 4h + 5min + 6s));
    CHECK(parseUtcIso8601("2001-02-03T04:05:06.5Z").value() == at(year{2001} / 2 / 3, 4h + 5min + 6s + 500ms));
    // Beyond the clock's 100 ns: rounded down (1,234,567 ticks of 100 ns).
    CHECK(parseUtcIso8601("2001-02-03T04:05:06.123456789Z").value() ==
          at(year{2001} / 2 / 3, 4h + 5min + 6s + system_clock::duration(1'234'567)));

    for (const char* text :
         {"", "2001-02-03", "2001-02-03T04:05:06", "2001-02-03 04:05:06Z", "2001-02-03T04:05:06+01:00",
          "2001-13-03T04:05:06Z", "2001-02-30T04:05:06Z", "2001-02-03T24:00:00Z", "2001-02-03T04:60:06Z",
          "2001-02-03T04:05:60Z", "2001-02-03T04:05:06.Z", "2001-02-03T04:05:06.1234567890Z", "2001-02-03T04:05:06Zx",
          "20o1-02-03T04:05:06Z"}) {
        INFO(text);
        const auto parsed = parseUtcIso8601(text);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == rv::ErrorCode::ParseError);
        CHECK(parsed.error().context == text);
    }
}
