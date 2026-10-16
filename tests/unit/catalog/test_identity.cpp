// GameIds (rv/catalog/identity.hpp, ADR-0008 §4) against golden values from an independent oracle: the table below is
// the output of `python -I scripts/game_id_oracle.py` (Python's hashlib on the documented tuple, nothing shared with
// rv_core's SHA-256), pasted as printed; `python -I scripts/game_id_oracle.py --check
// tests/unit/catalog/test_identity.cpp` finds every one of its lines here. Golden ids hold for keys seen the first
// time: once stored, an identity keeps its id whatever happens to the key.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <rv/catalog/identity.hpp>
#include <rv/core/sha256.hpp>
#include <rv/ui/quick_catalog_model.hpp>

using rv::catalog::AssignedGameId;
using rv::catalog::GameId;
using rv::catalog::GameIdRequest;
using rv::catalog::LibraryId;
using rv::catalog::SystemName;

namespace {

struct Golden {
    std::string_view library;
    std::string_view system;
    std::string_view key;
    std::uint32_t counter;
    std::string_view id;
};

// clang-format off
const Golden kGolden[] = {
{"esde", "gb", "TINY TENNIS (EUROPE).GB", 0, "g3c374d5e06145c9b"},
{"esde", "gb", "COLLECTION A/POCKET PUZZLE (WORLD).GB", 0, "g0fd5aa0df493bc1d"},
{"esde", "gb", "COLLECTION B/POCKET PUZZLE (WORLD).GB", 0, "g6c238f4311dd446c"},
{"esde", "gb", "CAF\xC3\x89 RACER (EUROPE).GB", 0, "g46cec009cda8f625"},
{"esde", "gb", ".DOT HERO (WORLD).GB", 0, "g24708bbee1686d96"},
{"esde", "snes", "ALPHA QUEST (USA).SFC", 0, "g7dde4199c42eb215"},
{"esde", "psx", "TWIN TRACKS (EUROPE).CUE", 0, "g57a9b27f3450d4d2"},
{"main", "gb", "TINY TENNIS (EUROPE).GB", 0, "g1828d8b8279612de"},
{"main", "gb", "COLLECTION A/POCKET PUZZLE (WORLD).GB", 0, "g16cb8340ef1ff95e"},
{"main", "gb", "COLLECTION B/POCKET PUZZLE (WORLD).GB", 0, "g234de8a910348ad6"},
{"main", "gb", "CAF\xC3\x89 RACER (EUROPE).GB", 0, "g77a3fb629faddbab"},
{"main", "gb", ".DOT HERO (WORLD).GB", 0, "g12ecbc3b83d8c9c1"},
{"main", "snes", "ALPHA QUEST (USA).SFC", 0, "g1b0ea5de8f99e9f4"},
{"main", "psx", "TWIN TRACKS (EUROPE).CUE", 0, "g5f3220af89646609"},
{"esde", "gbc", "TINY TENNIS (EUROPE).GB", 0, "g7147fd9ddca7eda8"},
{"esde", "gb", "STRA\xC3\x9F" "E (GERMANY).GB", 0, "g34103f51fc96fdfd"},
{"esde", "gb", "CAFE\xCC\x81 RACER (EUROPE).GB", 0, "g091ac2a166848b51"},
{"esde", "gb", "\xF0\x9F\x8E\xAE ARCADE (WORLD).GB", 0, "g354ddb4c8ee6202e"},
{"esde", "gb", "\xF0\x90\x90\xA8 LONG I.GB", 0, "g125bbcb8dea974f7"},
{"esde", "gb", "TINY TENNIS (EUROPE).GB", 1, "g263729a3e0a74672"},
{"esde", "gb", "TINY TENNIS (EUROPE).GB", 2, "g713baf7c66bc77c3"},
{"esde", "gb", "TINY TENNIS (EUROPE).GB", 10, "g12bd8046f0ba5f4d"},
};
// clang-format on

/// The golden id of a tuple.
GameId golden(std::string_view library, std::string_view system, std::string_view key, std::uint32_t counter) {
    const auto found = std::find_if(std::begin(kGolden), std::end(kGolden), [&](const Golden& row) {
        return row.library == library && row.system == system && row.key == key && row.counter == counter;
    });
    INFO("the golden id of " << library << "/" << system << "/" << key << "#" << counter);
    REQUIRE(found != std::end(kGolden));
    return GameId::parse(found->id).value();
}

LibraryId library(std::string_view id) {
    return LibraryId::parse(id).value();
}

SystemName systemNamed(std::string_view name) {
    return SystemName::parse(name).value();
}

GameIdRequest request(std::string_view systemName, std::string_view relativePath) {
    return GameIdRequest{systemNamed(systemName), rv::catalog::makeLookupKey(relativePath).value()};
}

/// The games of the standard fixture with a golden id, in (system, key, relative path) order.
std::vector<GameIdRequest> fixtureRequests() {
    std::vector<GameIdRequest> requests{
        request("snes", "Alpha Quest (USA).sfc"),
        request("gb", "Tiny Tennis (Europe).gb"),
        request("gb", "Collection B/Pocket Puzzle (World).gb"),
        request("psx", "Twin Tracks (Europe).cue"),
        request("gb", ".Dot Hero (World).gb"),
        request("gb", "Collection A/Pocket Puzzle (World).gb"),
        request("gb", "Caf\xC3\xA9 Racer (Europe).gb"),
    };
    std::sort(requests.begin(), requests.end(), [](const GameIdRequest& a, const GameIdRequest& b) {
        return std::tie(a.system, a.key) < std::tie(b.system, b.key);
    });
    return requests;
}

const rv::catalog::GameIdTaken kNothingTaken = [](GameId) {
    return false;
};

} // namespace

TEST_CASE("catalog/identity: the oracle's golden GameIds", "[catalog][identity]") {
    std::set<GameId> distinct;
    for (const Golden& row : kGolden) {
        INFO(row.library << "/" << row.system << "/" << row.key << "#" << row.counter);
        const auto id = rv::catalog::stableGameId(library(row.library), systemNamed(row.system), row.key, row.counter);
        REQUIRE(id.has_value());
        CHECK(id->toString() == row.id);
        CHECK(id->value() ==
              rv::catalog::gameIdCandidate(library(row.library), systemNamed(row.system), row.key, row.counter));
        CHECK(id->value() <= GameId::kMaxValue);
        CHECK(rv::ui::isValidEntryId(id->toString()));
        distinct.insert(*id);
    }
    CHECK(distinct.size() == std::size(kGolden));
}

TEST_CASE("catalog/identity: the tuple's bytes", "[catalog][identity]") {
    // The elements in UTF-8 separated by NUL characters, the counter in decimal after a fourth NUL; the first 8 bytes
    // of the digest big-endian, the top bit cleared.
    const auto valueOf = [](std::initializer_list<std::string_view> elements) {
        std::string tuple;
        for (const std::string_view element : elements) {
            tuple += element;
            tuple += '\0';
        }
        tuple.pop_back();
        const rv::core::Sha256Digest digest = rv::core::sha256(tuple);
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value = (value << 8) | digest[i];
        }
        return value & 0x7FFF'FFFF'FFFF'FFFFull;
    };
    CHECK(valueOf({"rv-game/1", "esde", "gb", "TINY TENNIS (EUROPE).GB"}) ==
          golden("esde", "gb", "TINY TENNIS (EUROPE).GB", 0).value());
    CHECK(valueOf({"rv-game/1", "esde", "gb", "TINY TENNIS (EUROPE).GB", "10"}) ==
          golden("esde", "gb", "TINY TENNIS (EUROPE).GB", 10).value());
    CHECK(rv::catalog::kGameIdTupleTag == "rv-game/1");
}

TEST_CASE("catalog/identity: the keys of the fixture's paths are the oracle's", "[catalog][identity]") {
    // clang-format off
    const std::pair<std::string_view, std::string_view> paths[] = {
        {"Tiny Tennis (Europe).gb", "TINY TENNIS (EUROPE).GB"},
        {"Collection A/Pocket Puzzle (World).gb", "COLLECTION A/POCKET PUZZLE (WORLD).GB"},
        {"Caf\xC3\xA9 Racer (Europe).gb", "CAF\xC3\x89 RACER (EUROPE).GB"},
        {".Dot Hero (World).gb", ".DOT HERO (WORLD).GB"},
        {"Alpha Quest (USA).sfc", "ALPHA QUEST (USA).SFC"},
        {"Twin Tracks (Europe).cue", "TWIN TRACKS (EUROPE).CUE"},
        {"Stra\xC3\x9F" "e (Germany).gb", "STRA\xC3\x9F" "E (GERMANY).GB"},
        {"Cafe\xCC\x81 Racer (Europe).gb", "CAFE\xCC\x81 RACER (EUROPE).GB"},
        {"\xF0\x9F\x8E\xAE Arcade (World).gb", "\xF0\x9F\x8E\xAE ARCADE (WORLD).GB"},
        {"\xF0\x90\x90\xA8 Long I.gb", "\xF0\x90\x90\xA8 LONG I.GB"},
    };
    // clang-format on
    for (const auto& [path, key] : paths) {
        INFO(path);
        CHECK(rv::catalog::makeLookupKey(path).value().key() == key);
        CHECK(std::any_of(std::begin(kGolden), std::end(kGolden), [&](const Golden& row) { return row.key == key; }));
    }
}

TEST_CASE("catalog/identity: a batch of new keys gets its tuples' values", "[catalog][identity]") {
    const std::vector<GameIdRequest> requests = fixtureRequests();
    const auto assigned = rv::catalog::assignGameIds(library("esde"), requests, kNothingTaken);
    REQUIRE(assigned.has_value());
    REQUIRE(assigned->size() == requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        INFO(requests[i].key.relativePath());
        CHECK((*assigned)[i] == AssignedGameId{golden("esde", requests[i].system.str(), requests[i].key.key(), 0), 0});
    }
    // Deterministic: the same batch, the same ids.
    CHECK(rv::catalog::assignGameIds(library("esde"), requests, kNothingTaken).value() == *assigned);
}

TEST_CASE("catalog/identity: two libraries give disjoint ids for the same keys", "[catalog][identity]") {
    const std::vector<GameIdRequest> requests = fixtureRequests();
    const auto esde = rv::catalog::assignGameIds(library("esde"), requests, kNothingTaken).value();
    const auto main = rv::catalog::assignGameIds(library("main"), requests, kNothingTaken).value();
    std::set<GameId> esdeIds;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        CHECK(main[i].id == golden("main", requests[i].system.str(), requests[i].key.key(), 0));
        esdeIds.insert(esde[i].id);
    }
    for (const AssignedGameId& id : main) {
        CHECK_FALSE(esdeIds.contains(id.id));
    }
    // The same key in another system is another tuple too.
    CHECK(rv::catalog::stableGameId(library("esde"), systemNamed("gbc"), "TINY TENNIS (EUROPE).GB").value() !=
          golden("esde", "gb", "TINY TENNIS (EUROPE).GB", 0));
}

TEST_CASE("catalog/identity: a collision with a taken id tries the next counters", "[catalog][identity]") {
    const std::vector<GameIdRequest> requests = fixtureRequests();
    const auto tiny = std::find_if(requests.begin(), requests.end(), [](const GameIdRequest& r) {
        return r.key.relativePath() == "Tiny Tennis (Europe).gb";
    });
    REQUIRE(tiny != requests.end());
    const auto tinyIndex = static_cast<std::size_t>(tiny - requests.begin());
    const std::string_view tinyKey = "TINY TENNIS (EUROPE).GB";

    // The tuple's own value is taken (by an identity that moved here, say): counter 1, the others untouched.
    std::set<GameId> taken{golden("esde", "gb", tinyKey, 0)};
    const rv::catalog::GameIdTaken isTaken = [&](GameId id) {
        return taken.contains(id);
    };
    auto assigned = rv::catalog::assignGameIds(library("esde"), requests, isTaken).value();
    CHECK(assigned[tinyIndex] == AssignedGameId{golden("esde", "gb", tinyKey, 1), 1});
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (i != tinyIndex) {
            CHECK(assigned[i].counter == 0);
        }
    }
    // Counters 0 and 1 taken: counter 2.
    taken.insert(golden("esde", "gb", tinyKey, 1));
    assigned = rv::catalog::assignGameIds(library("esde"), requests, isTaken).value();
    CHECK(assigned[tinyIndex] == AssignedGameId{golden("esde", "gb", tinyKey, 2), 2});
    // Counters 0 to 9 taken: counter 10, written with two digits.
    for (std::uint32_t counter = 2; counter < 10; ++counter) {
        taken.insert(rv::catalog::stableGameId(library("esde"), systemNamed("gb"), tinyKey, counter).value());
    }
    assigned = rv::catalog::assignGameIds(library("esde"), requests, isTaken).value();
    CHECK(assigned[tinyIndex] == AssignedGameId{golden("esde", "gb", tinyKey, 10), 10});
}

TEST_CASE("catalog/identity: names that fold to one key get distinct ids in order", "[catalog][identity]") {
    // "TINY TENNIS (EUROPE).gb" and "Tiny Tennis (Europe).gb" share their key (a case-sensitive folder): the first in
    // (key, relative path) order gets the tuple's value, the next one counter 1.
    const std::vector<GameIdRequest> requests{request("gb", "TINY TENNIS (EUROPE).gb"),
                                              request("gb", "Tiny Tennis (Europe).gb")};
    const auto assigned = rv::catalog::assignGameIds(library("esde"), requests, kNothingTaken);
    REQUIRE(assigned.has_value());
    CHECK((*assigned)[0] == AssignedGameId{golden("esde", "gb", "TINY TENNIS (EUROPE).GB", 0), 0});
    CHECK((*assigned)[1] == AssignedGameId{golden("esde", "gb", "TINY TENNIS (EUROPE).GB", 1), 1});
}

TEST_CASE("catalog/identity: a value of 0 counts as a collision", "[catalog][identity]") {
    const auto candidate = [](std::uint32_t counter) -> std::uint64_t {
        switch (counter) {
        case 0:
            return 0;
        case 1:
            return 1ull << 63; // beyond 63 bits: no GameId either (gameIdCandidate() never gives it)
        default:
            return 1000 + counter;
        }
    };
    auto first = rv::catalog::firstFreeGameId(candidate, kNothingTaken);
    REQUIRE(first.has_value());
    CHECK(*first == AssignedGameId{GameId::fromValue(1002).value(), 2});

    const rv::catalog::GameIdTaken taken1002 = [](GameId id) {
        return id.value() == 1002;
    };
    first = rv::catalog::firstFreeGameId(candidate, taken1002);
    REQUIRE(first.has_value());
    CHECK(*first == AssignedGameId{GameId::fromValue(1003).value(), 3});

    const auto exhausted = rv::catalog::firstFreeGameId([](std::uint32_t) { return std::uint64_t{7}; },
                                                        [](GameId id) { return id.value() == 7; }, 3);
    REQUIRE_FALSE(exhausted.has_value());
    CHECK(exhausted.error().code == rv::ErrorCode::OutOfRange);
}

TEST_CASE("catalog/identity: malformed batches are refused", "[catalog][identity]") {
    const auto refused = [](const std::vector<GameIdRequest>& requests) {
        const auto assigned = rv::catalog::assignGameIds(library("esde"), requests, kNothingTaken);
        REQUIRE_FALSE(assigned.has_value());
        CHECK(assigned.error().code == rv::ErrorCode::InvalidArgument);
    };
    refused({request("snes", "a.sfc"), request("gb", "a.gb")});                         // systems out of order
    refused({request("gb", "b.gb"), request("gb", "a.gb")});                            // keys out of order
    refused({request("gb", "Tiny.gb"), request("gb", "TINY.gb")});                      // relative paths out of order
    refused({request("gb", "a.gb"), request("gb", "a.gb")});                            // a file twice
    refused({GameIdRequest{SystemName(), rv::catalog::makeLookupKey("a.gb").value()}}); // no system
    refused({GameIdRequest{systemNamed("gb"), rv::catalog::LookupKey()}});              // no key
    CHECK(rv::catalog::assignGameIds(library("esde"), {}, kNothingTaken).value().empty());
}
