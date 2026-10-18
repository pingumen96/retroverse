// The three fixtures of the generated room (I3 step 1c): snes-room, snes-single and snes-stress, each as its README
// counts it: materialized, read by the ROM scan and the library snapshot, their media found by esde::EsdeMediaResolver,
// every generated cover decoded with assets::decodeImage (or failing as its README says) and the room's clips played.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include <rv/assets/image_io.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/media/video_decoder.hpp>

#include "../media/media_test_support.hpp"
#include "snapshot_test_support.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::catalog::CatalogEntry;
using rv::catalog::EntryMedia;
using rv::catalog::EntryState;
using rv::catalog::LibrarySnapshot;
using rv::catalog::MediaKind;
using rv::test::esde::countsOf;
using rv::test::esde::entryAt;
using rv::test::esde::loadSnapshot;
using rv::test::esde::materialize;
using rv::test::esde::profileOf;
using rv::test::esde::StateCounts;

namespace {

/// The invented full name the three fixtures give their system.
constexpr std::string_view kFullName = "Lumen 16 Home System";

LibrarySnapshot load(const rv::test::Fixture& fixture) {
    rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {}});
    return loadSnapshot(source, profileOf(fixture));
}

rv::catalog::SystemName snes() {
    auto name = rv::catalog::SystemName::parse("snes");
    REQUIRE(name.has_value());
    return *name;
}

/// The media of every game of snes, by relative path.
std::map<std::string, EntryMedia> mediaOf(const rv::test::Fixture& fixture, const LibrarySnapshot& snapshot) {
    rv::esde::EsdeMediaResolver resolver(fixture.path("media"));
    std::map<std::string, EntryMedia> media;
    for (const CatalogEntry& entry : snapshot.entries) {
        if (entry.state != EntryState::Game) {
            continue;
        }
        auto presence = resolver.presence(entry.system, entry.key, false);
        INFO(entry.key.relativePath());
        REQUIRE(presence.has_value());
        media.emplace(std::string(entry.key.relativePath()), *presence);
    }
    return media;
}

std::size_t countKind(const std::map<std::string, EntryMedia>& media, MediaKind kind) {
    return static_cast<std::size_t>(
        std::count_if(media.begin(), media.end(), [&](const auto& item) { return item.second[kind].present; }));
}

/// The path of an entry's media file of `kind`.
fs::path mediaFile(const rv::test::Fixture& fixture, std::string_view relativePath, MediaKind kind,
                   const EntryMedia& media) {
    const rv::esde::EsdeMediaResolver resolver(fixture.path("media"));
    auto key = rv::catalog::makeLookupKey(relativePath);
    REQUIRE(key.has_value());
    auto path = resolver.path(snes(), *key, false, kind, media[kind]);
    REQUIRE(path.has_value());
    return *path;
}

/// What decodeImage says of a file: ok, or the error's code.
std::optional<rv::ErrorCode> decodeFailure(const fs::path& file) {
    const auto bytes = rv::core::readBinaryFile(file);
    REQUIRE(bytes.has_value());
    const auto image = rv::assets::decodeImage(*bytes, 0, rv::core::pathToUtf8(file));
    if (image) {
        CHECK(image->width > 0);
        return std::nullopt;
    }
    return image.error().code;
}

rv::media::DecodeFailure clipEnd(const fs::path& clip) {
    rv::media::DecoderSettings settings;
    settings.loop = false;
    auto decoder = rv::test::startDecoder(clip, settings);
    (void)rv::test::collectFrames(*decoder, 10'000);
    REQUIRE(rv::test::waitUntil([&] {
        return decoder->state() == rv::media::DecoderState::Failed ||
               decoder->state() == rv::media::DecoderState::Ended;
    }));
    return decoder->failure();
}

/// The games that have a cover, and the title keys among them (the title lower-cased, the region and other tags left
/// out): what room selection (ADR-0010 §3) will count as eligible and as titles.
struct Eligible {
    std::size_t games = 0;
    std::set<std::string> titles;
};

Eligible eligibleOf(const LibrarySnapshot& snapshot, const std::map<std::string, EntryMedia>& media) {
    Eligible eligible;
    for (const CatalogEntry& entry : snapshot.entries) {
        if (entry.state != EntryState::Game ||
            !media.at(std::string(entry.key.relativePath()))[MediaKind::Cover].present) {
            continue;
        }
        ++eligible.games;
        std::string title = entry.metadata.name.empty() ? entry.title : entry.metadata.name;
        if (const std::size_t group = title.find(" ("); group != std::string::npos) {
            title.erase(group);
        }
        eligible.titles.insert(rv::core::toLowerAscii(title));
    }
    return eligible;
}

void checkSystem(const LibrarySnapshot& snapshot) {
    REQUIRE(snapshot.systems.size() == 1);
    CHECK(snapshot.systems[0].name.str() == "snes");
    CHECK(snapshot.systems[0].fullName == kFullName);
}

} // namespace

TEST_CASE("esde/snes_fixtures: snes-room", "[esde][fixture][snes]") {
    const rv::test::Fixture fixture = materialize("esde/snes-room");
    const LibrarySnapshot snapshot = load(fixture);
    checkSystem(snapshot);
    CHECK(countsOf(snapshot, "snes") == StateCounts{55, 55, 0, 0, 0, 0, 0});
    CHECK(std::count_if(snapshot.entries.begin(), snapshot.entries.end(),
                        [](const CatalogEntry& entry) { return entry.fromGamelist(); }) == 53);

    const std::map<std::string, EntryMedia> media = mediaOf(fixture, snapshot);
    CHECK(countKind(media, MediaKind::Cover) == 52);
    CHECK(countKind(media, MediaKind::PhysicalMedia) == 10);
    CHECK(countKind(media, MediaKind::Screenshot) == 4);
    CHECK(countKind(media, MediaKind::Video) == 14);
    for (const MediaKind kind : {MediaKind::BackCover, MediaKind::Box3d, MediaKind::TitleScreen, MediaKind::Marquee,
                                 MediaKind::MixImage, MediaKind::FanArt}) {
        CHECK(countKind(media, kind) == 1);
    }
    CHECK(countKind(media, MediaKind::Manual) == 0);
    CHECK(countKind(media, MediaKind::Custom) == 0);

    // Room selection's view: 52 games with a cover, 50 titles; the two European releases are duplicates, the game
    // without a cover and the two hacks without media are left out.
    const Eligible eligible = eligibleOf(snapshot, media);
    CHECK(eligible.games == 52);
    CHECK(eligible.titles.size() == 50);
    CHECK(media.at("Cobalt Compass (Europe).sfc")[MediaKind::Cover].present);
    CHECK(media.at("Gilded Gazebo (Europe).sfc")[MediaKind::Cover].present);
    CHECK_FALSE(media.at("Thistle Trellis (USA).sfc")[MediaKind::Cover].present);
    CHECK(media.at("Thistle Trellis (USA).sfc")[MediaKind::Video].present);
    CHECK_FALSE(media.at("Amber Lantern Night Edition (Hack).sfc").any());
    CHECK_FALSE(media.at("Fennel Ferry (USA) [h1].sfc").any());
    CHECK(entryAt(snapshot, "snes", "Fennel Ferry (USA) [h1].sfc").title == "Fennel Ferry (USA) [h1]");

    // Priority data: six favorites, play counts on 26 games.
    std::size_t favorites = 0;
    std::size_t played = 0;
    for (const CatalogEntry& entry : snapshot.entries) {
        favorites += entry.flags.favorite ? 1 : 0;
        played += entry.flags.playCount > 0 ? 1 : 0;
    }
    CHECK(favorites == 6);
    CHECK(played == 26);
    CHECK(entryAt(snapshot, "snes", "Kiln Keeper - The Dappled Almanac (USA).sfc").metadata.name ==
          "Kiln Keeper: The Dappled Almanac");

    // Every cover decodes, but the two damaged on purpose.
    std::map<std::string, rv::ErrorCode> failures;
    std::size_t portrait = 0;
    std::size_t jpeg = 0;
    for (const auto& [path, entryMedia] : media) {
        if (!entryMedia[MediaKind::Cover].present) {
            continue;
        }
        const fs::path cover = mediaFile(fixture, path, MediaKind::Cover, entryMedia);
        jpeg += entryMedia[MediaKind::Cover].extension == ".jpg" ? 1 : 0;
        if (const auto failure = decodeFailure(cover)) {
            failures.emplace(path, *failure);
        } else if (const auto image = rv::assets::loadImage(cover); image->height > image->width) {
            ++portrait;
        }
    }
    CHECK(failures == std::map<std::string, rv::ErrorCode>{{"Bellows Brigade (USA).sfc", rv::ErrorCode::ParseError},
                                                           {"Parasol Patrol (USA).sfc", rv::ErrorCode::Unsupported}});
    CHECK(portrait == 1);
    CHECK(jpeg == 8);
    // The cartridge pictures keep their transparency; the screenshot of Bramble Burrow Rally is cut short.
    const auto cartridge = rv::assets::loadImage(
        mediaFile(fixture, "Amber Lantern (USA).sfc", MediaKind::PhysicalMedia, media.at("Amber Lantern (USA).sfc")));
    REQUIRE(cartridge.has_value());
    CHECK(cartridge->channels == 4);
    CHECK(decodeFailure(mediaFile(fixture, "Bramble Burrow Rally (USA).sfc", MediaKind::Screenshot,
                                  media.at("Bramble Burrow Rally (USA).sfc"))) == rv::ErrorCode::ParseError);

    // The clips: ten that play, four damaged ones; Pewter Parasol has none (the missing clip).
    std::map<std::string, rv::media::DecodeFailure> clipFailures;
    std::size_t played4x3 = 0;
    for (const auto& [path, entryMedia] : media) {
        if (!entryMedia[MediaKind::Video].present) {
            continue;
        }
        const fs::path clip = mediaFile(fixture, path, MediaKind::Video, entryMedia);
        const rv::media::DecodeFailure failure = clipEnd(clip);
        if (failure != rv::media::DecodeFailure::None) {
            clipFailures.emplace(path, failure);
            continue;
        }
        const auto probed = rv::media::probeVideo(clip);
        REQUIRE(probed.has_value());
        played4x3 += probed->codedSize.width * 3 == probed->codedSize.height * 4 ? 1 : 0;
    }
    CHECK(played4x3 == 10);
    CHECK(clipFailures == std::map<std::string, rv::media::DecodeFailure>{
                              {"Lilac Loom (USA).sfc", rv::media::DecodeFailure::CorruptData},
                              {"Mossy Meadow Mystery (USA).sfc", rv::media::DecodeFailure::Truncated},
                              {"Nutmeg Nook (USA).sfc", rv::media::DecodeFailure::InvalidFile},
                              {"Opal Oriel (Europe).sfc", rv::media::DecodeFailure::UnsupportedCodec}});
    CHECK(media.at("Pewter Parasol (USA).sfc")[MediaKind::Cover].present);
    CHECK_FALSE(media.at("Pewter Parasol (USA).sfc")[MediaKind::Video].present);
    CHECK(media.at("Fennel Ferry (USA).sfc")[MediaKind::Video].extension == ".mkv");
    CHECK(media.at("Gilded Gazebo (USA).sfc")[MediaKind::Video].extension == ".avi");
}

TEST_CASE("esde/snes_fixtures: snes-room with G10's ten games added", "[esde][fixture][snes]") {
    const rv::test::Fixture fixture = materialize("esde/snes-room");
    // The additions' gamelist is the first one with ten entries more: the first 53 alike.
    const auto first = rv::core::readTextFile(fixture.path("ES-DE/gamelists/snes/gamelist.xml"));
    const auto later = rv::core::readTextFile(fixture.path("additions/ES-DE/gamelists/snes/gamelist.xml"));
    REQUIRE(first.has_value());
    REQUIRE(later.has_value());
    const std::string firstEntries = first->substr(0, first->rfind("</gameList>"));
    CHECK(later->starts_with(firstEntries));

    REQUIRE(fixture.applyOverlay("additions"));
    const LibrarySnapshot snapshot = load(fixture);
    CHECK(countsOf(snapshot, "snes") == StateCounts{65, 65, 0, 0, 0, 0, 0});
    const std::map<std::string, EntryMedia> media = mediaOf(fixture, snapshot);
    CHECK(countKind(media, MediaKind::Cover) == 62);
    const Eligible eligible = eligibleOf(snapshot, media);
    CHECK(eligible.games == 62);
    CHECK(eligible.titles.size() == 60);
    for (const std::string_view added : {"Zinnia Zephyr (USA).sfc", "Abacus Avenue (USA).sfc"}) {
        INFO(added);
        CHECK(entryAt(snapshot, "snes", added).fromGamelist());
        CHECK(media.at(std::string(added))[MediaKind::Cover].present);
    }
}

TEST_CASE("esde/snes_fixtures: snes-single", "[esde][fixture][snes]") {
    const rv::test::Fixture fixture = materialize("esde/snes-single");
    const LibrarySnapshot snapshot = load(fixture);
    checkSystem(snapshot);
    CHECK(countsOf(snapshot, "snes") == StateCounts{1, 1, 0, 0, 0, 0, 0});
    const CatalogEntry& game = entryAt(snapshot, "snes", "Pocket Lighthouse (USA).sfc");
    CHECK(game.metadata.name == "Pocket Lighthouse");
    // The media root is there and empty: media are known, and there are none.
    CHECK(fs::is_directory(fixture.path("media")));
    CHECK(fs::is_empty(fixture.path("media")));
    rv::esde::EsdeMediaResolver resolver(fixture.path("media"));
    CHECK(resolver.checkRoot());
    const auto presence = resolver.presence(snes(), game.key, false);
    REQUIRE(presence.has_value());
    CHECK_FALSE(presence->any());
}

TEST_CASE("esde/snes_fixtures: snes-stress", "[esde][fixture][snes]") {
    const rv::test::Fixture fixture = materialize("esde/snes-stress");

    const LibrarySnapshot snapshot = load(fixture);
    checkSystem(snapshot);
    CHECK(countsOf(snapshot, "snes") == StateCounts{2300, 2300, 0, 0, 0, 0, 0});
    CHECK(std::all_of(snapshot.entries.begin(), snapshot.entries.end(),
                      [](const CatalogEntry& entry) { return entry.fromGamelist(); }));

    const std::map<std::string, EntryMedia> media = mediaOf(fixture, snapshot);
    CHECK(countKind(media, MediaKind::Cover) == 2300);
    CHECK(countKind(media, MediaKind::Video) == 4);
    const Eligible eligible = eligibleOf(snapshot, media);
    CHECK(eligible.games == 2300);
    CHECK(eligible.titles.size() == 2000);

    std::size_t europe = 0;
    std::size_t revisions = 0;
    std::size_t favorites = 0;
    for (const CatalogEntry& entry : snapshot.entries) {
        const std::string_view path = entry.key.relativePath();
        revisions += path.ends_with("(Rev 1).sfc") ? 1 : 0;
        europe += path.ends_with("(Europe).sfc") ? 1 : 0;
        favorites += entry.flags.favorite ? 1 : 0;
    }
    CHECK(revisions == 100);
    CHECK(europe == 300); // the 200 second releases and the 100 titles released in Europe alone
    CHECK(favorites == 80);

    // Every cover decodes (in Debug, where stb_image is slow, every JPEG and one PNG in 10).
#if defined(NDEBUG)
    constexpr std::size_t kPngSample = 1;
#else
    constexpr std::size_t kPngSample = 10;
#endif
    std::size_t decoded = 0;
    std::size_t portrait = 0;
    std::set<std::string> extensions;
    std::size_t index = 0;
    for (const auto& [path, entryMedia] : media) {
        if (index++ % kPngSample != 0 && entryMedia[MediaKind::Cover].extension == ".png") {
            continue;
        }
        extensions.insert(entryMedia[MediaKind::Cover].extension);
        const auto image = rv::assets::loadImage(mediaFile(fixture, path, MediaKind::Cover, entryMedia));
        INFO(path);
        REQUIRE(image.has_value());
        portrait += image->height > image->width ? 1 : 0;
        ++decoded;
    }
    CHECK(decoded >= 2300 / kPngSample);
    if constexpr (kPngSample == 1) {
        CHECK(portrait == 60); // 40 titles, 20 of them with a second release
    }
    CHECK(extensions == std::set<std::string>{".jpg", ".png"});
}

// The roadmap's bound (I3 step 1c), hidden and meant for Release on a quiet machine:
//   build/msvc-x64-release/bin/rv_tests.exe "esde/bench: snes-stress*"
// The wall time depends on the machine's other work; the CPU time the process spent says what it costs.
TEST_CASE("esde/bench: snes-stress is materialized within 10 s", "[.bench]") {
    const auto cpuSeconds = [] {
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        REQUIRE(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
        const auto ticks = [](const FILETIME& time) {
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        return static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
    };
    const double cpuBefore = cpuSeconds();
    const auto started = std::chrono::steady_clock::now();
    const rv::test::Fixture fixture = materialize("esde/snes-stress");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double cpu = cpuSeconds() - cpuBefore;
    WARN(fmt::format("Informative timing (I3 step 1c): snes-stress materialized in {:.2f} s ({:.1f} s of CPU time on "
                     "{} hardware threads)",
                     seconds, cpu, std::thread::hardware_concurrency()));
    CHECK(seconds <= 10.0);
}
