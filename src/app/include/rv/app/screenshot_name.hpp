#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <string>

namespace rv::app {

/// Local calendar date and time of day, to the millisecond.
struct LocalTimestamp {
    int year = 1970;
    int month = 1; ///< 1..12
    int day = 1;   ///< 1..31
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;
};

/// The current time of the system clock in the local time zone.
[[nodiscard]] LocalTimestamp currentLocalTimestamp();

/// File name of an interactive (F12) screenshot taken at `time`: "retroverse-YYYYMMDD-HHMMSS-mmm.png". Names sort in
/// chronological order, two screenshots in the same second get different names, and no character is reserved by
/// Windows file names.
[[nodiscard]] std::string screenshotFileName(const LocalTimestamp& time);

} // namespace rv::app
