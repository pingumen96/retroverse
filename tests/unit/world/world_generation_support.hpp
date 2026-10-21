#pragma once

// What the generator's tests share (I3 step 3): the shipped world data for the SNES room, read once from the source
// tree's assets, and generated room selections: invented titles (syllables, never a real title) with deterministic
// priority data (favourites, play counts, ratings) and a few unplaced games, built from a seed and sorted by
// priorityLess() as world::selectRoom (I3 step 2) returns them.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/random.hpp>
#include <rv/core/seed.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/room_selection.hpp>

namespace rv::test::world {

inline const std::filesystem::path& assetRoot() {
    static const std::filesystem::path root = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return root;
}

inline const rv::catalog::SystemName& snes() {
    static const rv::catalog::SystemName system = *rv::catalog::SystemName::parse("snes");
    return system;
}

inline const rv::catalog::LibraryId& library() {
    static const rv::catalog::LibraryId id = *rv::catalog::LibraryId::parse("esde");
    return id;
}

/// The shipped data of the SNES room, read once.
inline const rv::world::WorldData& snesData() {
    static const rv::world::WorldData data = [] {
        auto loaded = rv::world::loadWorldData(assetRoot(), snes());
        REQUIRE(loaded.has_value());
        return std::move(*loaded);
    }();
    return data;
}

/// An invented title of two or three syllable words.
inline std::string inventedTitle(rv::core::Random& random) {
    static constexpr const char* kSyllables[] = {"ka", "lo", "mi", "ra", "zen", "to", "vex", "nu", "pa", "shi",
                                                 "do", "re", "qua", "bel", "xo", "fi", "gar", "lun", "mor", "sti"};
    constexpr std::uint64_t kCount = sizeof(kSyllables) / sizeof(kSyllables[0]);
    std::string title;
    const std::uint64_t words = 2 + random.below(2);
    for (std::uint64_t word = 0; word < words; ++word) {
        if (word > 0) {
            title += ' ';
        }
        const std::uint64_t syllables = 1 + random.below(3);
        for (std::uint64_t syllable = 0; syllable < syllables; ++syllable) {
            title += kSyllables[random.below(kCount)];
        }
    }
    return title;
}

/// A selection of `titles` titles for the SNES room and titles / 20 unplaced games (duplicate titles and games without
/// a cover), from `seed`: about 1 in 20 titles a favourite, 1 in 3 played, 7 in 10 rated; `titles` distinct titles over
/// all its games (the games without a cover counted as versions of titles).
inline rv::world::RoomSelection makeSelection(std::size_t titles, std::uint64_t seed) {
    rv::core::Random random(rv::core::deriveSeed("test-selection", seed));
    rv::world::RoomSelection selection;
    selection.room = *rv::world::RoomId::forSystem(snes());
    selection.eligibility = rv::world::Eligibility::Covered;
    selection.titleKeyRule = "title-key/1";
    selection.catalogSortKeyVersion = rv::catalog::kSortKeyVersion;
    std::set<std::uint64_t> ids;
    std::set<std::string> keys;
    const auto newGame = [&] {
        for (;;) {
            const std::uint64_t value = random.next() & rv::catalog::GameId::kMaxValue;
            if (value != 0 && ids.insert(value).second) {
                return *rv::catalog::GameId::fromValue(value);
            }
        }
    };
    for (std::size_t i = 0; i < titles; ++i) {
        rv::world::RoomTitle title;
        title.game = newGame();
        std::string key = inventedTitle(random);
        while (!keys.insert(key).second) {
            key += " " + std::to_string(random.below(100));
        }
        title.titleKey = key;
        title.sortKey = key;
        title.favorite = random.below(20) == 0;
        title.playCount = random.below(3) == 0 ? 1 + random.below(60) : 0;
        if (random.below(10) < 7) {
            title.ratingTenths = static_cast<int>(random.below(11));
        }
        selection.titles.push_back(std::move(title));
    }
    std::sort(selection.titles.begin(), selection.titles.end(), rv::world::priorityLess);
    for (std::size_t i = 0; i < titles / 20; ++i) {
        rv::world::UnplacedGame game;
        game.game = newGame();
        game.room = selection.room;
        if (i % 2 == 0) {
            game.reason = rv::world::UnplacedReason::DuplicateTitle;
            game.representative = selection.titles[random.below(titles)].game;
        } else {
            game.reason = rv::world::UnplacedReason::NoCover;
        }
        selection.unplaced.push_back(game);
    }
    std::sort(selection.unplaced.begin(), selection.unplaced.end());
    selection.distinctTitles = selection.titles.size();
    return selection;
}

/// The configuration of the room suites (I3's, ADR-0013 §5): every room a console room of its own whatever its size.
inline rv::world::WorldConfig roomConfig(std::uint64_t worldSeed) {
    rv::world::WorldConfig config;
    config.seed = worldSeed;
    config.sharedRoomMinTitles = 0;
    return config;
}

/// A fresh plan of `titles` titles of the selection of `selectionSeed`, with the world seed `worldSeed`, in a console
/// room whatever its size (roomConfig()).
inline rv::world::GenerationResult generateRoom(std::size_t titles, std::uint64_t selectionSeed,
                                                std::uint64_t worldSeed) {
    rv::world::GenerationInput input;
    input.library = library();
    input.selection = makeSelection(titles, selectionSeed);
    input.config = roomConfig(worldSeed);
    auto result = rv::world::generate(input, snesData());
    REQUIRE(result.has_value());
    return std::move(*result);
}

} // namespace rv::test::world
