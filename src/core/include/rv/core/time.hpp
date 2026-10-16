#pragma once

// UTC wall-clock times as ISO-8601 text: when play sessions start and end (ADR-0008 §9), when imports ran.

#include <chrono>
#include <string>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::core {

/// "YYYY-MM-DDTHH:MM:SS.mmmZ": UTC, milliseconds (the time rounded down), always 24 characters for the years 0000 to
/// 9999, so that the text sorts like the times. Years outside that range are written with a sign ("+12345-...").
[[nodiscard]] std::string formatUtcIso8601(std::chrono::system_clock::time_point time);

/// The current UTC wall-clock time as formatUtcIso8601() writes it.
[[nodiscard]] std::string utcNow();

/// Reads "YYYY-MM-DDTHH:MM:SS[.fraction]Z" back (the 'T' and the 'Z' required, 1 to 9 fraction digits, years 0000 to
/// 9999; a fraction beyond the clock's precision is rounded down). Errors: ParseError naming the text.
[[nodiscard]] Expected<std::chrono::system_clock::time_point> parseUtcIso8601(std::string_view text);

} // namespace rv::core
