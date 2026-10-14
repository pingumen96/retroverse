#include <rv/app/screenshot_name.hpp>

#include <chrono>
#include <ctime>

#include <fmt/format.h>

namespace rv::app {

LocalTimestamp currentLocalTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto sinceEpoch = now.time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        sinceEpoch - std::chrono::floor<std::chrono::seconds>(sinceEpoch));
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    LocalTimestamp timestamp;
    timestamp.year = local.tm_year + 1900;
    timestamp.month = local.tm_mon + 1;
    timestamp.day = local.tm_mday;
    timestamp.hour = local.tm_hour;
    timestamp.minute = local.tm_min;
    timestamp.second = local.tm_sec;
    timestamp.millisecond = static_cast<int>(milliseconds.count());
    return timestamp;
}

std::string screenshotFileName(const LocalTimestamp& time) {
    return fmt::format("retroverse-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.png", time.year, time.month, time.day,
                       time.hour, time.minute, time.second, time.millisecond);
}

} // namespace rv::app
