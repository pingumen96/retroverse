#include <rv/core/time.hpp>

#include <cstdint>
#include <optional>

#include <fmt/format.h>

namespace rv::core {
namespace {

/// The decimal number of `count` digits at `position`, nothing when one of them is not a digit.
std::optional<int> digitsAt(std::string_view text, std::size_t position, std::size_t count) {
    if (position + count > text.size()) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t i = position; i < position + count; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return std::nullopt;
        }
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

} // namespace

std::string formatUtcIso8601(std::chrono::system_clock::time_point time) {
    using namespace std::chrono;
    const auto instant = floor<milliseconds>(time);
    const auto day = floor<days>(instant);
    const year_month_day date{day};
    const hh_mm_ss<milliseconds> clock{instant - day};
    const int year = static_cast<int>(date.year());
    const std::string yearText = year >= 0 && year <= 9999 ? fmt::format("{:04}", year) : fmt::format("{:+05}", year);
    return fmt::format("{}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", yearText, static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()), clock.hours().count(), clock.minutes().count(),
                       clock.seconds().count(), clock.subseconds().count());
}

std::string utcNow() {
    return formatUtcIso8601(std::chrono::system_clock::now());
}

Expected<std::chrono::system_clock::time_point> parseUtcIso8601(std::string_view text) {
    using namespace std::chrono;
    const auto fail = [&](std::string_view why) {
        return makeError(ErrorCode::ParseError,
                         "not a UTC time of the form YYYY-MM-DDTHH:MM:SS[.fraction]Z: " + std::string(why),
                         std::string(text));
    };
    // YYYY-MM-DDTHH:MM:SS is 19 characters.
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':') {
        return fail("unexpected layout");
    }
    const auto year = digitsAt(text, 0, 4);
    const auto month = digitsAt(text, 5, 2);
    const auto dayOfMonth = digitsAt(text, 8, 2);
    const auto hour = digitsAt(text, 11, 2);
    const auto minute = digitsAt(text, 14, 2);
    const auto second = digitsAt(text, 17, 2);
    if (!year || !month || !dayOfMonth || !hour || !minute || !second) {
        return fail("a field is not a number");
    }
    const year_month_day date{std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
                              std::chrono::day{static_cast<unsigned>(*dayOfMonth)}};
    if (!date.ok()) {
        return fail("no such date");
    }
    if (*hour > 23 || *minute > 59 || *second > 59) {
        return fail("no such time of day");
    }

    std::size_t position = 19;
    nanoseconds fraction{0};
    if (text[position] == '.') {
        ++position;
        std::size_t digits = 0;
        std::int64_t value = 0;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            if (digits == 9) {
                return fail("more than 9 fraction digits");
            }
            value = value * 10 + (text[position] - '0');
            ++digits;
            ++position;
        }
        if (digits == 0) {
            return fail("a decimal point without digits");
        }
        for (std::size_t i = digits; i < 9; ++i) {
            value *= 10;
        }
        fraction = nanoseconds(value);
    }
    if (position + 1 != text.size() || text[position] != 'Z') {
        return fail("it must end with Z (UTC)");
    }
    const auto instant = sys_days{date} + hours{*hour} + minutes{*minute} + seconds{*second};
    return time_point_cast<system_clock::duration>(instant) + floor<system_clock::duration>(fraction);
}

} // namespace rv::core
