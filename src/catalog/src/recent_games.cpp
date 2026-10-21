#include <rv/catalog/recent_games.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace rv::catalog {
namespace {

using Seconds = std::int64_t;

constexpr Seconds kDay = 86'400;
/// The most intervals a converter keeps (two a year with daylight saving time: centuries of them).
constexpr std::size_t kMaxIntervals = 4'096;
/// A time farther than this from the intervals kept starts them again at its own (a date centuries away, such as a
/// hand-edited <lastplayed> of 9999, is not worth reading every interval in between).
constexpr Seconds kMaxGap = 100 * 366 * kDay;

/// A local wall time as seconds since 1970-01-01T00:00:00 of the same wall clock; nothing for a date that does not
/// exist (a LocalDateTime made by hand: parseEsdeDateTime() refuses them).
std::optional<Seconds> localSecondsOf(const LocalDateTime& time) {
    const std::chrono::year_month_day date{std::chrono::year{time.year},
                                           std::chrono::month{static_cast<unsigned>(time.month)},
                                           std::chrono::day{static_cast<unsigned>(time.day)}};
    if (!date.ok() || time.hour < 0 || time.hour > 23 || time.minute < 0 || time.minute > 59 || time.second < 0 ||
        time.second > 59) {
        return std::nullopt;
    }
    const std::chrono::local_seconds local = std::chrono::local_days{date} + std::chrono::hours{time.hour} +
                                             std::chrono::minutes{time.minute} + std::chrono::seconds{time.second};
    return local.time_since_epoch().count();
}

UtcTime utcOf(Seconds seconds) {
    return UtcTime(std::chrono::duration_cast<UtcTime::duration>(std::chrono::seconds{seconds}));
}

/// The intervals of a zone's rules read so far, contiguous and in order: each [begin, end) in UTC seconds with its
/// offset east of UTC. The zone database is asked only for the intervals a conversion needs that are not read yet; a
/// conversion far from them (kMaxGap), or one that would grow them past kMaxIntervals, starts them again at its own
/// time, so that no time locks the others out.
class ZoneIntervals {
public:
    explicit ZoneIntervals(const std::chrono::time_zone& zone) : m_zone(&zone) {}

    /// std::chrono::time_zone::to_sys(local, choose::earliest), to the second.
    std::optional<UtcTime> convert(const LocalDateTime& time) {
        const std::optional<Seconds> local = localSecondsOf(time);
        if (!local) {
            return std::nullopt;
        }
        try {
            // An offset is less than a day either way: every interval whose wall clock can show `local` meets
            // [local - 1 day, local + 1 day] in UTC.
            if (!cover(*local - kDay, *local + kDay)) {
                return std::nullopt;
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
        // The intervals whose wall clock shows `local`: one (a unique time), two (a time shown twice: the earlier
        // instant, the first interval's), none (a time skipped: the instant of the change, the next interval's
        // begin). Comparisons written so that no sum reaches an interval's open end (the zone's first and last
        // intervals reach the time point's limits).
        for (const Interval& interval : m_intervals) {
            const Seconds utc = *local - interval.offset;
            if (utc >= interval.begin && utc < interval.end) {
                return utcOf(utc);
            }
        }
        for (const Interval& interval : m_intervals) {
            if (interval.begin > *local - interval.offset) {
                return utcOf(interval.begin);
            }
        }
        return std::nullopt;
    }

private:
    struct Interval {
        Seconds begin = 0;
        Seconds end = 0;
        Seconds offset = 0;
    };

    [[nodiscard]] Interval read(Seconds at) const {
        const std::chrono::sys_info info = m_zone->get_info(std::chrono::sys_seconds{std::chrono::seconds{at}});
        return Interval{info.begin.time_since_epoch().count(), info.end.time_since_epoch().count(),
                        std::chrono::duration_cast<std::chrono::seconds>(info.offset).count()};
    }

    enum class Grown : std::uint8_t { Covered, Full, Broken };

    /// Reads the intervals that cover [from, to]: false when the zone gives intervals that do not join up (a broken
    /// database). The intervals kept start again at [from, to] when it lies farther than kMaxGap from them, or when
    /// reaching it would keep more than kMaxIntervals.
    [[nodiscard]] bool cover(Seconds from, Seconds to) {
        // Written so that no difference overflows: the zone's first and last intervals reach the time point's limits.
        if (!m_intervals.empty() &&
            ((from > m_intervals.back().end && from - m_intervals.back().end > kMaxGap) ||
             (to < m_intervals.front().begin && m_intervals.front().begin - to > kMaxGap))) {
            m_intervals.clear();
        }
        switch (grow(from, to)) {
        case Grown::Covered:
            return true;
        case Grown::Broken:
            return false;
        case Grown::Full:
            m_intervals.clear();
            return grow(from, to) == Grown::Covered;
        }
        return false;
    }

    [[nodiscard]] Grown grow(Seconds from, Seconds to) {
        if (m_intervals.empty()) {
            m_intervals.push_back(read(from));
        }
        while (m_intervals.front().begin > from) {
            if (m_intervals.size() >= kMaxIntervals) {
                return Grown::Full;
            }
            const Interval before = read(m_intervals.front().begin - 1);
            if (before.end != m_intervals.front().begin || before.begin >= before.end) {
                return Grown::Broken;
            }
            m_intervals.push_front(before);
        }
        while (m_intervals.back().end <= to) {
            if (m_intervals.size() >= kMaxIntervals) {
                return Grown::Full;
            }
            const Interval after = read(m_intervals.back().end);
            if (after.begin != m_intervals.back().end || after.begin >= after.end) {
                return Grown::Broken;
            }
            m_intervals.push_back(after);
        }
        return Grown::Covered;
    }

    const std::chrono::time_zone* m_zone;
    std::deque<Interval> m_intervals;
};

} // namespace

LocalTimeToUtc zoneLocalTimeToUtc(const std::chrono::time_zone& zone) {
    auto intervals = std::make_shared<ZoneIntervals>(zone);
    return [intervals](const LocalDateTime& time) { return intervals->convert(time); };
}

Expected<LocalTimeToUtc> machineLocalTimeToUtc() {
    try {
        const std::chrono::time_zone* zone = std::chrono::current_zone();
        if (zone == nullptr) {
            return makeError(ErrorCode::Unavailable, "the machine's time zone is not known");
        }
        return zoneLocalTimeToUtc(*zone);
    } catch (const std::exception& e) {
        return makeError(ErrorCode::Unavailable, "the time zone database cannot be read", e.what());
    }
}

LocalTimeToUtc fixedOffsetLocalTimeToUtc(std::chrono::minutes offset) {
    const Seconds offsetSeconds = std::chrono::duration_cast<std::chrono::seconds>(offset).count();
    return [offsetSeconds](const LocalDateTime& time) -> std::optional<UtcTime> {
        const std::optional<Seconds> local = localSecondsOf(time);
        if (!local) {
            return std::nullopt;
        }
        return utcOf(*local - offsetSeconds);
    };
}

bool recentGameBefore(const RecentGame& a, const RecentGame& b) noexcept {
    if (a.favorite != b.favorite) {
        return a.favorite;
    }
    if (a.lastPlayed != b.lastPlayed) {
        if (!a.lastPlayed || !b.lastPlayed) {
            return a.lastPlayed.has_value();
        }
        return *a.lastPlayed > *b.lastPlayed;
    }
    if (a.sortKey != b.sortKey) {
        return a.sortKey < b.sortKey;
    }
    return a.id < b.id;
}

void rankRecentGames(std::vector<RecentGame>& candidates, const LocalTimeToUtc& localToUtc, std::size_t limit) {
    for (RecentGame& game : candidates) {
        game.lastPlayed = game.lastSession;
        if (game.esdeLastPlayed && localToUtc) {
            const std::optional<UtcTime> esde = localToUtc(*game.esdeLastPlayed);
            if (esde && (!game.lastPlayed || *esde > *game.lastPlayed)) {
                game.lastPlayed = esde;
            }
        }
    }
    if (limit < candidates.size()) {
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(limit),
                          candidates.end(), recentGameBefore);
        candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(limit), candidates.end());
    } else {
        std::sort(candidates.begin(), candidates.end(), recentGameBefore);
    }
}

} // namespace rv::catalog
