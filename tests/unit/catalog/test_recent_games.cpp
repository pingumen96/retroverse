// I4 step 7a: the rule of the directory's favourites and recent games (rv/catalog/recent_games.hpp): ES-DE's local
// <lastplayed> converted to UTC by a zone's rules (the converter's interval cache against std::chrono's own to_sys with
// choose::earliest, across daylight saving changes of zones with whole-hour, half-hour and no changes, and over decades;
// a time shown twice is its earlier instant, a skipped one the instant of the change), by a fixed offset, by the
// machine's zone; the order (favourites first, the most recent, the never played last, the sort key, the GameId) and
// the ranking of candidates (the later of a session and <lastplayed>, the limit). The query on the database is in
// tests/unit/catalog_db/test_recent_query.cpp.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/recent_games.hpp>
#include <rv/core/random.hpp>
#include <rv/core/time.hpp>

using namespace rv::catalog;
using namespace std::chrono_literals;

namespace {

LocalDateTime local(int year, int month, int day, int hour, int minute = 0, int second = 0) {
    return LocalDateTime{year, month, day, hour, minute, second};
}

UtcTime utc(std::string_view text) {
    return rv::core::parseUtcIso8601(text).value();
}

/// The zone named `name` from the time zone database; nothing when the machine has no database (the test skips).
const std::chrono::time_zone* zoneNamed(std::string_view name) {
    try {
        return std::chrono::locate_zone(name);
    } catch (const std::exception&) {
        return nullptr;
    }
}

/// What std::chrono itself answers: time_zone::to_sys with choose::earliest.
UtcTime reference(const std::chrono::time_zone& zone, const LocalDateTime& time) {
    const std::chrono::year_month_day date{std::chrono::year{time.year},
                                           std::chrono::month{static_cast<unsigned>(time.month)},
                                           std::chrono::day{static_cast<unsigned>(time.day)}};
    const std::chrono::local_seconds wall = std::chrono::local_days{date} + std::chrono::hours{time.hour} +
                                            std::chrono::minutes{time.minute} + std::chrono::seconds{time.second};
    return zone.to_sys(wall, std::chrono::choose::earliest);
}

/// `time` advanced by `minutes` on the wall clock (a calendar walk, no zone).
LocalDateTime plusMinutes(const LocalDateTime& time, int minutes) {
    const std::chrono::year_month_day date{std::chrono::year{time.year},
                                           std::chrono::month{static_cast<unsigned>(time.month)},
                                           std::chrono::day{static_cast<unsigned>(time.day)}};
    const std::chrono::local_seconds wall = std::chrono::local_days{date} + std::chrono::hours{time.hour} +
                                            std::chrono::minutes{time.minute + minutes} +
                                            std::chrono::seconds{time.second};
    const auto days = std::chrono::floor<std::chrono::days>(wall);
    const std::chrono::year_month_day next{days};
    const std::chrono::hh_mm_ss clock{wall - days};
    return LocalDateTime{static_cast<int>(next.year()), static_cast<int>(static_cast<unsigned>(next.month())),
                         static_cast<int>(static_cast<unsigned>(next.day())), static_cast<int>(clock.hours().count()),
                         static_cast<int>(clock.minutes().count()), static_cast<int>(clock.seconds().count())};
}

RecentGame game(std::uint64_t id, std::string_view sortKey, bool favorite = false) {
    RecentGame recent;
    recent.id = GameId::fromValue(id).value();
    recent.system = SystemName::parse("snes").value();
    recent.title = std::string(sortKey);
    recent.sortKey = std::string(sortKey);
    recent.favorite = favorite;
    return recent;
}

std::vector<std::uint64_t> idsOf(const std::vector<RecentGame>& games) {
    std::vector<std::uint64_t> ids;
    for (const RecentGame& recent : games) {
        ids.push_back(recent.id.value());
    }
    return ids;
}

} // namespace

TEST_CASE("catalog/recent_games: a zone's wall time to UTC as std::chrono's to_sys with choose::earliest",
          "[catalog][recent_games]") {
    for (const std::string_view name : {"Europe/Rome", "America/New_York", "Australia/Lord_Howe", "Asia/Kolkata",
                                        "Pacific/Chatham", "UTC"}) {
        INFO(name);
        const std::chrono::time_zone* zone = zoneNamed(name);
        if (zone == nullptr) {
            SKIP("the machine has no time zone database");
        }
        const LocalTimeToUtc convert = zoneLocalTimeToUtc(*zone);
        // Every 5 minutes of the wall clock from a day before to a day after each change of 2026 (the hours shown
        // twice and the hours skipped among them), then times spread over decades in random order (the interval cache
        // read backwards and forwards).
        std::vector<LocalDateTime> times;
        const auto yearEnd = std::chrono::sys_days{std::chrono::year{2027} / 1 / 1};
        for (std::chrono::sys_seconds at = std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}; at < yearEnd;) {
            const std::chrono::sys_info info = zone->get_info(at);
            if (info.end >= yearEnd) {
                break;
            }
            const auto wall = std::chrono::floor<std::chrono::days>(info.end + info.offset - 24h);
            const std::chrono::year_month_day day{wall};
            const LocalDateTime start = local(static_cast<int>(day.year()), static_cast<int>(static_cast<unsigned>(day.month())),
                                              static_cast<int>(static_cast<unsigned>(day.day())), 0);
            for (int minutes = 0; minutes <= 3 * 24 * 60; minutes += 5) {
                times.push_back(plusMinutes(start, minutes));
            }
            at = info.end;
        }
        rv::core::Random random(20261021);
        for (int sample = 0; sample < 500; ++sample) {
            times.push_back(LocalDateTime{static_cast<int>(random.between(1975, 2045)),
                                          static_cast<int>(random.between(1, 12)),
                                          static_cast<int>(random.between(1, 28)),
                                          static_cast<int>(random.between(0, 23)),
                                          static_cast<int>(random.between(0, 59)),
                                          static_cast<int>(random.between(0, 59))});
        }
        for (const LocalDateTime& time : times) {
            const std::optional<UtcTime> converted = convert(time);
            INFO(formatEsdeDateTime(time));
            REQUIRE(converted.has_value());
            CHECK(*converted == reference(*zone, time));
        }
    }
}

TEST_CASE("catalog/recent_games: a wall time shown twice is its earlier instant, a skipped one the change's",
          "[catalog][recent_games]") {
    const std::chrono::time_zone* rome = zoneNamed("Europe/Rome");
    if (rome == nullptr) {
        SKIP("the machine has no time zone database");
    }
    const LocalTimeToUtc convert = zoneLocalTimeToUtc(*rome);
    // Winter (UTC+1) and summer (UTC+2).
    CHECK(convert(local(2026, 1, 15, 20, 30)) == utc("2026-01-15T19:30:00Z"));
    CHECK(convert(local(2026, 7, 15, 20, 30)) == utc("2026-07-15T18:30:00Z"));
    // 25 October 2026: the clocks go back from 03:00 to 02:00; 02:30 is shown twice, first at 00:30 UTC.
    CHECK(convert(local(2026, 10, 25, 2, 30)) == utc("2026-10-25T00:30:00Z"));
    CHECK(convert(local(2026, 10, 25, 3, 0)) == utc("2026-10-25T02:00:00Z"));
    // 29 March 2026: the clocks go forward from 02:00 to 03:00; 02:30 is never shown: the change, 01:00 UTC.
    CHECK(convert(local(2026, 3, 29, 2, 30)) == utc("2026-03-29T01:00:00Z"));
    CHECK(convert(local(2026, 3, 29, 3, 0)) == utc("2026-03-29T01:00:00Z"));
    CHECK(convert(local(2026, 3, 29, 1, 59, 59)) == utc("2026-03-29T00:59:59Z"));
    // A date that does not exist (built by hand: ES-DE's reader refuses it) converts to nothing.
    CHECK_FALSE(convert(local(2026, 2, 30, 12)).has_value());
}

TEST_CASE("catalog/recent_games: a date centuries away does not lock the converter's other dates out",
          "[catalog][recent_games]") {
    const std::chrono::time_zone* rome = zoneNamed("Europe/Rome");
    if (rome == nullptr) {
        SKIP("the machine has no time zone database");
    }
    // A hand-edited <lastplayed> of 9999 (parseEsdeDateTime() takes it) between ordinary dates, in both orders, and a
    // date before the zone's rules: each converts as std::chrono's to_sys does, whatever was converted before it.
    const auto check = [&](const LocalTimeToUtc& convert, const LocalDateTime& time) {
        INFO(formatEsdeDateTime(time));
        std::optional<UtcTime> expected;
        try {
            expected = reference(*rome, time);
        } catch (const std::exception&) {
            // The zone database does not reach that date: the converter says nothing either.
        }
        CHECK(convert(time) == expected);
    };
    const LocalDateTime ordinary[]{local(2026, 10, 1, 20), local(2024, 5, 1, 12), local(2026, 3, 29, 2, 30),
                                   local(1999, 12, 31, 23, 59)};
    const LocalDateTime far[]{local(9999, 12, 31, 0), local(1850, 6, 1, 12)};
    for (const LocalDateTime& first : far) {
        const LocalTimeToUtc farFirst = zoneLocalTimeToUtc(*rome);
        check(farFirst, first);
        for (const LocalDateTime& time : ordinary) {
            check(farFirst, time);
        }
        const LocalTimeToUtc farBetween = zoneLocalTimeToUtc(*rome);
        check(farBetween, ordinary[0]);
        check(farBetween, first);
        for (const LocalDateTime& time : ordinary) {
            check(farBetween, time);
        }
        check(farBetween, first);
        check(farBetween, ordinary[1]);
    }
    // Dates walking away less than the restart distance each time, past the intervals a converter keeps (two a year
    // here), then one before them all: still std::chrono's answers.
    const LocalTimeToUtc walk = zoneLocalTimeToUtc(*rome);
    for (int year = 2026; year <= 4400; year += 90) {
        check(walk, local(year, 7, 1, 12));
    }
    check(walk, local(2024, 5, 1, 12));
    check(walk, local(2026, 10, 25, 2, 30));
}

TEST_CASE("catalog/recent_games: a fixed offset and the machine's zone", "[catalog][recent_games]") {
    CHECK(fixedOffsetLocalTimeToUtc(120min)(local(2026, 10, 1, 20, 0)) == utc("2026-10-01T18:00:00Z"));
    CHECK(fixedOffsetLocalTimeToUtc(-330min)(local(2026, 10, 1, 20, 0)) == utc("2026-10-02T01:30:00Z"));
    CHECK(fixedOffsetLocalTimeToUtc(0min)(local(2000, 2, 29, 23, 59, 59)) == utc("2000-02-29T23:59:59Z"));
    CHECK_FALSE(fixedOffsetLocalTimeToUtc(0min)(local(2001, 2, 29, 0)).has_value());

    auto machine = machineLocalTimeToUtc();
    if (!machine) {
        SKIP("the machine's time zone cannot be read: " << machine.error().toString());
    }
    const std::chrono::time_zone& zone = *std::chrono::current_zone();
    for (const LocalDateTime& time : {local(2026, 1, 15, 12), local(2026, 7, 15, 12), local(1999, 12, 31, 23, 59)}) {
        CHECK((*machine)(time) == reference(zone, time));
    }
}

TEST_CASE("catalog/recent_games: favourites first, then the most recent, the never played last, then sort key and "
          "GameId",
          "[catalog][recent_games]") {
    RecentGame favouriteNever = game(9, "zeta", true);
    RecentGame favouriteOld = game(8, "alpha", true);
    favouriteOld.lastPlayed = utc("2020-01-01T00:00:00Z");
    RecentGame favouriteNew = game(7, "omega", true);
    favouriteNew.lastPlayed = utc("2026-10-01T00:00:00Z");
    RecentGame playedNew = game(1, "beta");
    playedNew.lastPlayed = utc("2026-10-20T00:00:00Z");
    RecentGame playedSameA = game(5, "delta");
    playedSameA.lastPlayed = utc("2026-10-10T00:00:00Z");
    RecentGame playedSameB = game(4, "delta");
    playedSameB.lastPlayed = utc("2026-10-10T00:00:00Z");
    RecentGame playedSameC = game(3, "charlie");
    playedSameC.lastPlayed = utc("2026-10-10T00:00:00Z");
    RecentGame neverPlayed = game(2, "aaa");

    std::vector<RecentGame> byHand{neverPlayed, playedSameA, favouriteNever, playedNew,   playedSameB,
                                   favouriteOld, playedSameC, favouriteNew};
    std::sort(byHand.begin(), byHand.end(), recentGameBefore);
    CHECK(idsOf(byHand) == std::vector<std::uint64_t>{7, 8, 9, 1, 3, 4, 5, 2});
    CHECK_FALSE(recentGameBefore(playedNew, playedNew));
}

TEST_CASE("catalog/recent_games: the later of the last session and ES-DE's lastplayed, ranked and cut at the limit",
          "[catalog][recent_games]") {
    const LocalTimeToUtc rome = fixedOffsetLocalTimeToUtc(120min);
    // A session at 23:45 UTC against a <lastplayed> of 01:30 the next day at UTC+2 (23:30 UTC): the session is later.
    RecentGame session = game(1, "session");
    session.lastSession = utc("2026-10-04T23:45:00Z");
    RecentGame esde = game(2, "esde");
    esde.esdeLastPlayed = local(2026, 10, 5, 1, 30);
    // Both: the later of the two counts.
    RecentGame both = game(3, "both");
    both.lastSession = utc("2026-09-01T10:00:00Z");
    both.esdeLastPlayed = local(2026, 10, 6, 9, 0);
    RecentGame favourite = game(4, "favourite", true);
    favourite.esdeLastPlayed = local(1999, 1, 1, 0, 0);

    std::vector<RecentGame> games{session, esde, both, favourite};
    rankRecentGames(games, rome, 10);
    CHECK(idsOf(games) == std::vector<std::uint64_t>{4, 3, 1, 2});
    CHECK(games[0].lastPlayed == utc("1998-12-31T22:00:00Z"));
    CHECK(games[1].lastPlayed == utc("2026-10-06T07:00:00Z"));
    CHECK(games[2].lastPlayed == utc("2026-10-04T23:45:00Z"));
    CHECK(games[3].lastPlayed == utc("2026-10-04T23:30:00Z"));

    // Read as UTC, the same <lastplayed> is 01:30 UTC: later than the session.
    games = {session, esde, both, favourite};
    rankRecentGames(games, fixedOffsetLocalTimeToUtc(0min), 10);
    CHECK(idsOf(games) == std::vector<std::uint64_t>{4, 3, 2, 1});

    // Without a converter <lastplayed> counts for nothing: a game played only in ES-DE is last.
    games = {session, esde, both, favourite};
    rankRecentGames(games, {}, 10);
    CHECK(idsOf(games) == std::vector<std::uint64_t>{4, 1, 3, 2});
    CHECK_FALSE(games[0].lastPlayed.has_value());

    games = {session, esde, both, favourite};
    rankRecentGames(games, rome, 2);
    CHECK(idsOf(games) == std::vector<std::uint64_t>{4, 3});
    rankRecentGames(games, rome, 0);
    CHECK(games.empty());
}
