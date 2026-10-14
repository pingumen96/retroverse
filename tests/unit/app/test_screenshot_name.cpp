#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <rv/app/screenshot_name.hpp>

using rv::app::LocalTimestamp;
using rv::app::screenshotFileName;

TEST_CASE("app/screenshot_name: names carry the local date, time and milliseconds", "[app][screenshot]") {
    LocalTimestamp time;
    time.year = 2026;
    time.month = 10;
    time.day = 14;
    time.hour = 23;
    time.minute = 5;
    time.second = 9;
    time.millisecond = 7;
    CHECK(screenshotFileName(time) == "retroverse-20261014-230509-007.png");

    time.month = 1;
    time.day = 2;
    time.hour = 0;
    time.minute = 0;
    time.second = 0;
    time.millisecond = 999;
    CHECK(screenshotFileName(time) == "retroverse-20260102-000000-999.png");
}

TEST_CASE("app/screenshot_name: names sort chronologically and avoid reserved characters", "[app][screenshot]") {
    std::vector<LocalTimestamp> times(4);
    times[0] = {2026, 9, 30, 23, 59, 59, 999};
    times[1] = {2026, 10, 1, 0, 0, 0, 0};
    times[2] = {2026, 10, 1, 0, 0, 0, 1};
    times[3] = {2026, 10, 1, 10, 0, 0, 0};
    std::vector<std::string> names;
    for (const LocalTimestamp& time : times) {
        names.push_back(screenshotFileName(time));
    }
    CHECK(std::is_sorted(names.begin(), names.end()));
    for (const std::string& name : names) {
        INFO(name);
        CHECK(name.find_first_of("<>:\"/\\|?* ") == std::string::npos);
    }
}

TEST_CASE("app/screenshot_name: the current local time is a valid calendar time", "[app][screenshot]") {
    const LocalTimestamp now = rv::app::currentLocalTimestamp();
    CHECK(now.year >= 2024);
    CHECK((now.month >= 1 && now.month <= 12));
    CHECK((now.day >= 1 && now.day <= 31));
    CHECK((now.hour >= 0 && now.hour <= 23));
    CHECK((now.minute >= 0 && now.minute <= 59));
    CHECK((now.second >= 0 && now.second <= 60)); // leap second
    CHECK((now.millisecond >= 0 && now.millisecond <= 999));
    const std::string name = screenshotFileName(now);
    CHECK(name.size() == std::string("retroverse-YYYYMMDD-HHMMSS-mmm.png").size());
}
