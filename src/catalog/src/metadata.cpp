#include <rv/catalog/metadata.hpp>

#include <array>
#include <limits>

#include <fmt/format.h>

#include <rv/core/case_fold.hpp>
#include <rv/core/text.hpp>

namespace rv::catalog {
namespace {

constexpr std::string_view kWhitespace = " \t\r\n\f\v";

bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool isWhitespace(char c) noexcept {
    return kWhitespace.find(c) != std::string_view::npos;
}

tl::unexpected<Error> parseError(std::string_view what, std::string_view text) {
    return makeError(ErrorCode::ParseError, "not " + std::string(what), std::string(text));
}

/// The decimal number of `digits` (digits only, at least one), or nothing beyond `max`.
std::optional<std::uint64_t> decimal(std::string_view digits, std::uint64_t max) noexcept {
    if (digits.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : digits) {
        if (!isDigit(c)) {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (max - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

bool isLeapYear(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month) noexcept {
    static constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && isLeapYear(year) ? 29 : kDays[static_cast<std::size_t>(month - 1)];
}

/// The text after the last '/' of a relative path.
std::string_view lastElement(std::string_view relativePath) noexcept {
    const std::size_t slash = relativePath.rfind('/');
    return slash == std::string_view::npos ? relativePath : relativePath.substr(slash + 1);
}

} // namespace

Expected<LocalDateTime> parseEsdeDateTime(std::string_view text) {
    const std::string_view trimmed = core::trim(text);
    constexpr std::string_view kForm = "an ES-DE date-time (YYYYMMDDTHHMMSS)";
    if (trimmed.size() != 15 || trimmed[8] != 'T') {
        return parseError(kForm, text);
    }
    const auto field = [&](std::size_t at, std::size_t length) -> int {
        const auto value = decimal(trimmed.substr(at, length), 9999);
        return value ? static_cast<int>(*value) : -1;
    };
    const LocalDateTime time{field(0, 4), field(4, 2), field(6, 2), field(9, 2), field(11, 2), field(13, 2)};
    if (time.year < 1 || time.month < 1 || time.month > 12 || time.day < 1 ||
        time.day > daysInMonth(time.year, time.month) || time.hour < 0 || time.hour > 23 || time.minute < 0 ||
        time.minute > 59 || time.second < 0 || time.second > 59) {
        return parseError(kForm, text);
    }
    return time;
}

std::string formatEsdeDateTime(const LocalDateTime& time) {
    return fmt::format("{:04}{:02}{:02}T{:02}{:02}{:02}", time.year, time.month, time.day, time.hour, time.minute,
                       time.second);
}

Expected<ReleaseDate> parseReleaseDate(std::string_view text) {
    const auto time = parseEsdeDateTime(text);
    if (!time) {
        return propagate(time.error());
    }
    return ReleaseDate{time->year, time->month, time->day};
}

Expected<Rating> parseRating(std::string_view text) {
    const std::string_view trimmed = core::trim(text);
    constexpr std::string_view kForm = "a rating (a decimal number from 0 to 1)";
    if (trimmed.empty() || (trimmed[0] != '0' && trimmed[0] != '1')) {
        return parseError(kForm, text);
    }
    const int whole = trimmed[0] - '0';
    if (trimmed.size() == 1) {
        return Rating{whole * 10};
    }
    const std::string_view fraction = trimmed.substr(2);
    if (trimmed[1] != '.' || fraction.empty()) {
        return parseError(kForm, text);
    }
    for (const char c : fraction) {
        if (!isDigit(c) || (whole == 1 && c != '0')) {
            return parseError(kForm, text); // a stray character, or a value above 1
        }
    }
    int tenths = whole * 10 + (fraction[0] - '0');
    if (fraction.size() > 1 && fraction[1] >= '5') {
        ++tenths; // the nearest tenth, halves up ("0.75" is 0.8, "0.95" is 1)
    }
    return Rating{tenths};
}

std::string formatRating(Rating rating) {
    if (rating.tenths <= 0) {
        return "0";
    }
    if (rating.tenths >= 10) {
        return "1";
    }
    return fmt::format("0.{}", rating.tenths);
}

Expected<Players> parsePlayers(std::string_view text) {
    const std::string_view trimmed = core::trim(text);
    constexpr std::string_view kForm = "a number of players (N, N-M or N+)";
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint32_t>::max();
    std::size_t end = 0;
    while (end < trimmed.size() && isDigit(trimmed[end])) {
        ++end;
    }
    const auto min = decimal(trimmed.substr(0, end), kMax);
    if (!min || *min == 0) {
        return parseError(kForm, text);
    }
    const std::string_view rest = trimmed.substr(end);
    if (rest.empty()) {
        return Players{static_cast<std::uint32_t>(*min), static_cast<std::uint32_t>(*min)};
    }
    if (rest == "+") {
        return Players{static_cast<std::uint32_t>(*min), std::nullopt};
    }
    if (rest[0] == '-') {
        const auto max = decimal(rest.substr(1), kMax);
        if (max && *max >= *min) {
            return Players{static_cast<std::uint32_t>(*min), static_cast<std::uint32_t>(*max)};
        }
    }
    return parseError(kForm, text);
}

std::string formatPlayers(const Players& players) {
    if (!players.max) {
        return fmt::format("{}+", players.min);
    }
    if (*players.max == players.min) {
        return fmt::format("{}", players.min);
    }
    return fmt::format("{}-{}", players.min, *players.max);
}

Expected<bool> parseEsdeBool(std::string_view text) {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    return parseError("an ES-DE boolean (true or false)", text);
}

Expected<std::uint64_t> parseEsdeCount(std::string_view text) {
    const auto value = decimal(core::trim(text), static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    if (!value) {
        return parseError("a count (decimal digits)", text);
    }
    return *value;
}

std::string displayText(std::string_view raw) {
    const std::string_view text = core::trim(raw);
    std::string shown;
    shown.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (!isWhitespace(text[i])) {
            shown += text[i++];
            continue;
        }
        std::size_t end = i;
        bool breaks = false;
        while (end < text.size() && isWhitespace(text[end])) {
            breaks = breaks || text[end] != ' ';
            ++end;
        }
        if (breaks) {
            shown += ' ';
        } else {
            shown.append(text.substr(i, end - i));
        }
        i = end;
    }
    return shown;
}

std::string titleFromFileStem(std::string_view relativePath, bool directory) {
    const std::string_view name = lastElement(relativePath);
    std::string_view stem = name;
    if (!directory) {
        if (const std::size_t dot = name.rfind('.'); dot != std::string_view::npos) {
            stem = name.substr(0, dot);
        }
    }
    const std::string_view title = core::trim(stem);
    return std::string(title.empty() ? core::trim(name) : title);
}

Expected<std::string> sortKeyOf(std::string_view title) {
    const std::string_view text = core::trim(title);
    std::string folded;
    folded.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!isWhitespace(text[i])) {
            folded += text[i];
        } else if (!isWhitespace(text[i - 1])) { // i > 0: the text does not start with white space
            folded += ' ';
        }
    }
    return core::invariantLowerCase(folded);
}

std::string defaultTitle(const GameMetadata& metadata, std::string_view relativePath, bool directory) {
    std::string title = displayText(metadata.name);
    return title.empty() ? titleFromFileStem(relativePath, directory) : title;
}

namespace {

// The gamelist elements, as ES-DE 3.4 names them.
constexpr std::array<std::string_view, 11> kNormalizedElements{
    "releasedate", "rating", "players",   "favorite", "hidden",     "kidgame",
    "completed",   "broken", "playcount", "playtime", "lastplayed",
};
constexpr std::array<std::string_view, 8> kTextElements{
    "path", "name", "sortname", "desc", "developer", "publisher", "genre", "altemulator",
};

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& names, std::string_view name) noexcept {
    for (const std::string_view candidate : names) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

} // namespace

bool isModelledGamelistElement(std::string_view name) noexcept {
    return contains(kTextElements, name) || contains(kNormalizedElements, name);
}

bool isNormalizedGamelistElement(std::string_view name) noexcept {
    return contains(kNormalizedElements, name);
}

} // namespace rv::catalog
