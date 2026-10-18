#pragma once

// A generated ES-DE collection (I3 step 1c): one system's ROM files, their gamelist and their media, from a seed and a
// few counts, for fixtures too large to list file by file (tests/fixtures/esde/snes-stress, 2,000 titles). files.json's
// "collections" names them (tests/support/fixture.hpp); the materializer writes what generateCollection() describes.
// Invented titles, companies and descriptions; nothing commercial. Deterministic: the same spec always gives the same
// collection, file names, gamelist and media alike (core::Random, splitmix64/1).
//
// The rules (what tests/fixtures/esde/snes-stress/README.md states and its tests assert):
//  - `titles` distinct titles, each title key (the title lower-cased) once: two to five words from fixed word lists,
//    some with a subtitle after ": " (the file name writes " - " for it), some with a sequel numeral, some long;
//  - title i's region: "(World)" when i % 20 == 7, "(Europe)" when i % 20 == 13, "(Japan)" when i % 40 == 19, else
//    "(USA)";
//  - `duplicates` second releases, of the titles whose index i has i % 20 in {1, 4, 9}, in index order: "(Europe)" for
//    1 and 4, "(USA) (Rev 1)" for 9 (No-Intro's highest revision: there the revision is the representative, the plain
//    "(USA)" file the duplicate); at most 3 per 20 titles;
//  - every ROM file (`romFolder`/<stem><romExtension>, `romSize` bytes, payloadBytes() seeded by its index) has a
//    gamelist entry (name, description, release date, developer, publisher, genre, players, rating; a favorite every
//    25th title, play counts on about a quarter) and a cover in `mediaFolder`/covers: 680 x 497 PNG, but a JPEG
//    (".jpg") when i % 10 == 3 and a portrait PNG (497 x 680) when i % 50 == 11 (i is the title's index; both releases
//    of a title share the picture's seed);
//  - the first `videos` titles have a 4:3 clip in `mediaFolder`/videos (".mp4", 4 s at 30 fps, 320 x 240).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <rv/core/expected.hpp>

#include "support/fixture_payloads.hpp"

namespace rv::test {

/// files.json's "collections" item.
struct CollectionSpec {
    std::uint64_t seed = 0;
    std::size_t titles = 0;
    std::size_t duplicates = 0;
    std::size_t videos = 0;
    /// Paths relative to the fixture, '/' between the elements.
    std::string romFolder;
    std::string romExtension;
    std::uint64_t romSize = 0;
    std::string gamelist;
    std::string mediaFolder;
};

/// One ROM file of a generated collection.
struct CollectionGame {
    /// The title's index (0 .. titles - 1); both releases of a title share it.
    std::size_t titleIndex = 0;
    /// The gamelist name.
    std::string title;
    /// "(USA)", "(Europe)", "(USA) (Rev 1)", ...
    std::string tags;
    /// The file's stem: the title, " - " for ": ", then the tags.
    std::string stem;
    /// The release room selection takes for its title (ADR-0010 §3: USA or World before Europe, then the highest
    /// revision); the other release of a duplicated title is its duplicate. Favorites, play counts and clips are the
    /// representatives'.
    bool representative = true;
    bool favorite = false;
    std::uint32_t playCount = 0;
    /// The payload's seed.
    std::uint64_t romSeed = 0;
    PicturePayload cover;
    /// Its cover's extension: ".png" or ".jpg".
    std::string coverExtension;
    bool hasVideo = false;
    ClipPayload video;
};

/// A generated collection: its ROM files in title index order (a title's second release after its first) and its
/// gamelist.
struct GeneratedCollection {
    std::vector<CollectionGame> games;
    /// ES-DE's gamelist.xml for `romFolder`.
    std::string gamelist;
};

/// Reads a "collections" item: {"seed", "titles", "duplicates", "videos", "romFolder", "romExtension", "romSize",
/// "gamelist", "mediaFolder"}, all required. Errors: ValidationFailed naming the key (more duplicates than 3 per 20
/// titles allow, more videos than titles, more than 20,000 titles, a ROM larger than 64 MiB), `context` prefixed.
[[nodiscard]] Expected<CollectionSpec> parseCollectionSpec(const nlohmann::json& value, std::string_view context);

/// The collection a spec describes.
[[nodiscard]] GeneratedCollection generateCollection(const CollectionSpec& spec);

} // namespace rv::test
