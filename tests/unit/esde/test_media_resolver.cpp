// ES-DE's media layout (rv/esde/media_resolver.hpp, I2 step 8): the extensions and their order as ES-DE 3.4.1 tries
// them, the paths derived from an entry's relative path, the standard fixture's media (every kind, the png and jpg
// pair, the video formats, the ignored flv, folder covers, namesakes in sub-folders, a dotted folder), then media trees
// written at test time: names in another case, every extension in ES-DE's order, non-ASCII, dotted and empty media
// names, hidden files, paths past MAX_PATH, folders named like media, a refresh; and media roots and folders that
// cannot be read (missing, a file, unresolved, an unmapped drive, folders that cannot be listed).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>

#include "support/file_system.hpp"
#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::ErrorCode;
using rv::Status;
using rv::catalog::EntryMedia;
using rv::catalog::MediaKind;
using rv::catalog::MediaPresence;
using rv::esde::EsdeMediaResolver;
using rv::esde::MediaFolderState;

namespace {

rv::catalog::SystemName systemNamed(std::string_view name) {
    auto parsed = rv::catalog::SystemName::parse(name);
    REQUIRE(parsed.has_value());
    return *parsed;
}

rv::catalog::LookupKey keyOf(std::string_view relativePath) {
    auto key = rv::catalog::makeLookupKey(relativePath);
    INFO(relativePath);
    REQUIRE(key.has_value());
    return *key;
}

rv::test::Fixture materializeStandard() {
    auto materialized = rv::test::Fixture::materialize("esde/standard");
    INFO((materialized ? "materialized" : materialized.error().toString()));
    REQUIRE(materialized.has_value());
    return std::move(*materialized);
}

/// The media of an entry, which must be known.
EntryMedia mediaOf(EsdeMediaResolver& resolver, std::string_view system, std::string_view relativePath,
                   bool directory = false) {
    const auto media = resolver.presence(systemNamed(system), keyOf(relativePath), directory);
    INFO(system << "/" << relativePath << ": " << (media ? "known" : media.error().toString()));
    REQUIRE(media.has_value());
    return *media;
}

/// The extensions an entry's media have, kind by kind ("" for an absent kind), in the order of kMediaKinds.
std::array<std::string, rv::catalog::kMediaKindCount> extensionsOf(const EntryMedia& media) {
    std::array<std::string, rv::catalog::kMediaKindCount> extensions;
    for (std::size_t i = 0; i < rv::catalog::kMediaKindCount; ++i) {
        CHECK(media.kinds[i].present == !media.kinds[i].extension.empty());
        extensions[i] = media.kinds[i].extension;
    }
    return extensions;
}

/// An EntryMedia with `extension` for each kind given.
EntryMedia withMedia(std::initializer_list<std::pair<MediaKind, std::string_view>> kinds) {
    EntryMedia media;
    for (const auto& [kind, extension] : kinds) {
        media[kind] = MediaPresence{true, std::string(extension)};
    }
    return media;
}

const rv::esde::SystemMediaListing& listingOf(EsdeMediaResolver& resolver, std::string_view system) {
    const auto listing = resolver.listing(systemNamed(system));
    INFO(system << ": " << (listing ? "listed" : listing.error().toString()));
    REQUIRE(listing.has_value());
    return **listing;
}

void write(const fs::path& file, std::string_view content = "media") {
    INFO(rv::core::pathToUtf8(file));
    REQUIRE(rv::test::writeFileAnyLength(file, content).has_value());
}

/// A drive letter with no drive behind it, or 0.
wchar_t unusedDriveLetter() {
    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
        if ((drives & (1u << (letter - L'A'))) == 0) {
            return letter;
        }
    }
    return 0;
}

bool hasIssue(const rv::esde::SystemMediaListing& listing, std::string_view code, std::string_view part) {
    for (const rv::catalog::Issue& issue : listing.issues) {
        if (issue.code == code && issue.reason.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("esde/media: the extensions ES-DE 3.4.1 reads, in the order it tries them", "[esde][media]") {
    // FileData::sImageExtensions, sVideoExtensions and getManualPath of the tag v3.4.1.
    const std::vector<std::string_view> images{".png", ".jpg", ".webp"};
    const std::vector<std::string_view> videos{".mp4", ".mkv", ".avi", ".wmv", ".mov", ".webm"};
    for (const MediaKind kind : rv::catalog::kMediaKinds) {
        INFO(rv::catalog::toString(kind));
        const auto extensions = rv::esde::mediaExtensions(kind);
        const std::vector<std::string_view> actual(extensions.begin(), extensions.end());
        if (kind == MediaKind::Video) {
            CHECK(actual == videos);
        } else if (kind == MediaKind::Manual) {
            CHECK(actual == std::vector<std::string_view>{".pdf"});
        } else {
            CHECK(actual == images); // custom included
        }
    }
}

TEST_CASE("esde/media: the path of an entry's media below the media root", "[esde][media]") {
    const auto gb = systemNamed("gb");
    using rv::esde::mediaRelativePath;
    CHECK(mediaRelativePath(gb, keyOf("Collection A/Pocket Puzzle (World).gb"), false, MediaKind::Cover, ".png") ==
          "gb/covers/Collection A/Pocket Puzzle (World).png");
    // Only the last extension goes; a folder keeps its whole name.
    CHECK(mediaRelativePath(systemNamed("ps3"), keyOf("BioShock (Europe).dec.iso"), false, MediaKind::Screenshot,
                            ".png") == "ps3/screenshots/BioShock (Europe).dec.png");
    CHECK(mediaRelativePath(systemNamed("snes"), keyOf("Sports Vol. 2"), true, MediaKind::Cover, ".png") ==
          "snes/covers/Sports Vol. 2.png");
    CHECK(mediaRelativePath(systemNamed("snes"), keyOf("Sports Vol. 2"), false, MediaKind::Cover, ".png") ==
          "snes/covers/Sports Vol.png");
    // Never trimmed; a name that is all extension leaves an empty media name, as ES-DE builds it.
    CHECK(mediaRelativePath(systemNamed("atari800"), keyOf("Ultima II - Galactic Disk .atr"), false,
                            MediaKind::TitleScreen, ".png") == "atari800/titlescreens/Ultima II - Galactic Disk .png");
    CHECK(mediaRelativePath(gb, keyOf("Sub/.gb"), false, MediaKind::Box3d, ".png") == "gb/3dboxes/Sub/.png");
    CHECK(mediaRelativePath(systemNamed("vic20"), keyOf("witching hour"), false, MediaKind::Video, ".mp4") ==
          "vic20/videos/witching hour.mp4");
    CHECK(mediaRelativePath(gb, keyOf("Tiny Tennis (Europe).gb"), false, MediaKind::Manual, ".pdf") ==
          "gb/manuals/Tiny Tennis (Europe).pdf");
}

TEST_CASE("esde/media: the standard fixture's media, kind by kind", "[esde][media]") {
    const rv::test::Fixture fixture = materializeStandard();
    const fs::path root = fixture.path("media");
    EsdeMediaResolver resolver(root);
    REQUIRE(resolver.checkRoot().has_value());

    // gb: png before jpg, a jpg screenshot, mp4, avi and mkv videos, a namesake in another sub-folder without media,
    // the missing file's cover, the games without media.
    CHECK(mediaOf(resolver, "gb", "Tiny Tennis (Europe).gb") ==
          withMedia({{MediaKind::Cover, ".png"}, {MediaKind::Screenshot, ".jpg"}, {MediaKind::Video, ".mp4"}}));
    CHECK(mediaOf(resolver, "gb", "Collection A/Pocket Puzzle (World).gb") == withMedia({{MediaKind::Cover, ".png"}}));
    CHECK(mediaOf(resolver, "gb", "Collection B/Pocket Puzzle (World).gb") == EntryMedia{});
    CHECK(mediaOf(resolver, "gb", "Gone Fishing (USA).gb") == withMedia({{MediaKind::Cover, ".png"}}));
    CHECK(mediaOf(resolver, "gb", "Café Racer (Europe).gb") == withMedia({{MediaKind::Video, ".avi"}}));
    CHECK(mediaOf(resolver, "gb", "LOUD CART (USA).GB") == withMedia({{MediaKind::Video, ".mkv"}}));
    for (const std::string_view none : {".Dot Hero (World).gb", "Hidden Gem (World).gb",
                                        "Collection A/Deep/Bottom Dweller (Japan).gbc", "Notes.txt"}) {
        CHECK(mediaOf(resolver, "gb", none) == EntryMedia{});
    }
    CHECK(mediaOf(resolver, "gb", "Collection A", true) == EntryMedia{});

    // snes: all 12 kinds for one game, the flv that ES-DE never reads, the folder covers named with the whole name.
    const EntryMedia alpha = mediaOf(resolver, "snes", "Alpha Quest (USA).sfc");
    CHECK(extensionsOf(alpha) == std::array<std::string, rv::catalog::kMediaKindCount>{".png", ".png", ".png", ".png",
                                                                                       ".jpg", ".jpg", ".png", ".png",
                                                                                       ".jpg", ".pdf", ".mov", ".png"});
    CHECK(alpha.presenceMask() == 0xFFFu);
    CHECK(mediaOf(resolver, "snes", "Beta Run (Europe).SMC") == EntryMedia{});
    CHECK(mediaOf(resolver, "snes", "Hacks", true) == withMedia({{MediaKind::Cover, ".png"}}));
    CHECK(mediaOf(resolver, "snes", "Sports Vol. 2", true) == withMedia({{MediaKind::Cover, ".png"}}));
    CHECK(mediaOf(resolver, "snes", "Sports Vol. 2", false) == EntryMedia{}); // "Sports Vol": no such cover
    for (const std::string_view none : {"Delta Drift (USA).sfc", "Mixed Case (USA).Sfc", "Hacks/Gamma Hack (Hack).sfc",
                                        "Sports Vol. 2/Ice Lanes (USA).sfc"}) {
        CHECK(mediaOf(resolver, "snes", none) == EntryMedia{});
    }

    // psx: the cue sheet's cover and video; its tracks have other names, so no media.
    CHECK(mediaOf(resolver, "psx", "Twin Tracks (Europe).cue") ==
          withMedia({{MediaKind::Cover, ".png"}, {MediaKind::Video, ".wmv"}}));
    CHECK(mediaOf(resolver, "psx", "Twin Tracks (Europe) (Track 1).bin") == EntryMedia{});
    CHECK(mediaOf(resolver, "psx", "Solo Disc (USA).chd") == EntryMedia{});

    // A system without a media folder has no media, which is no error.
    CHECK(mediaOf(resolver, "nes", "Anything (World).nes") == EntryMedia{});
    CHECK(listingOf(resolver, "nes").state == MediaFolderState::Missing);

    // The listings.
    const rv::esde::SystemMediaListing& gb = listingOf(resolver, "gb");
    CHECK(gb.state == MediaFolderState::Listed);
    CHECK(gb.folderName == "gb");
    CHECK(gb[MediaKind::Cover].state == MediaFolderState::Listed);
    CHECK(gb[MediaKind::Cover].media == 3);
    CHECK(gb[MediaKind::Cover].shadowedFiles == 1); // the jpg beside Tiny Tennis' png
    CHECK(gb[MediaKind::Screenshot].media == 1);
    CHECK(gb[MediaKind::Video].media == 3);
    CHECK(gb[MediaKind::Manual].state == MediaFolderState::Missing);
    CHECK(gb[MediaKind::Manual].folderName.empty());
    CHECK(gb.media() == 7);
    CHECK(hasIssue(gb, rv::esde::kMediaIssueShadowedFile, "gb/covers"));
    const rv::esde::SystemMediaListing& snes = listingOf(resolver, "snes");
    for (const MediaKind kind : rv::catalog::kMediaKinds) {
        CHECK(snes[kind].state == MediaFolderState::Listed);
    }
    CHECK(snes[MediaKind::Cover].media == 3);
    CHECK(snes[MediaKind::Video].media == 1);
    CHECK(snes[MediaKind::Video].ignored == std::vector<rv::esde::IgnoredExtension>{{".flv", 1}});
    CHECK(snes[MediaKind::Video].ignoredFiles() == 1);
    CHECK(hasIssue(snes, rv::esde::kMediaIssueIgnoredExtension, "\".flv\" in snes/videos"));
    const rv::esde::SystemMediaListing& psx = listingOf(resolver, "psx");
    CHECK(psx.media() == 2);
    CHECK(psx.issues.empty());
    CHECK(resolver.stats().systemsListed == 4); // gb, snes, psx and nes
    CHECK(resolver.stats().files == 25);        // every media file of the fixture

    // Paths are derived, never stored: the media root, the system's name, the kind's folder, the entry's media name.
    const auto pathOf = [&](std::string_view system, std::string_view relativePath, bool directory, MediaKind kind,
                            const EntryMedia& media) {
        const auto path = resolver.path(systemNamed(system), keyOf(relativePath), directory, kind, media[kind]);
        INFO(relativePath << ": " << (path ? rv::core::pathToUtf8(*path) : path.error().toString()));
        REQUIRE(path.has_value());
        CHECK(fs::exists(rv::core::extendedLengthPath(*path))); // the file the listing found
        return rv::core::pathToUtf8(*path);
    };
    const auto expected = [&](std::string_view relative) {
        return rv::core::pathToUtf8((root / rv::core::pathFromUtf8(relative)).make_preferred());
    };
    CHECK(pathOf("snes", "Alpha Quest (USA).sfc", false, MediaKind::Manual, alpha) ==
          expected("snes/manuals/Alpha Quest (USA).pdf"));
    CHECK(pathOf("snes", "Alpha Quest (USA).sfc", false, MediaKind::FanArt, alpha) ==
          expected("snes/fanart/Alpha Quest (USA).jpg"));
    CHECK(pathOf("snes", "Sports Vol. 2", true, MediaKind::Cover, mediaOf(resolver, "snes", "Sports Vol. 2", true)) ==
          expected("snes/covers/Sports Vol. 2.png"));
    CHECK(pathOf("gb", "Collection A/Pocket Puzzle (World).gb", false, MediaKind::Cover,
                 mediaOf(resolver, "gb", "Collection A/Pocket Puzzle (World).gb")) ==
          expected("gb/covers/Collection A/Pocket Puzzle (World).png"));
    CHECK(pathOf("gb", "Tiny Tennis (Europe).gb", false, MediaKind::Cover,
                 mediaOf(resolver, "gb", "Tiny Tennis (Europe).gb")) == expected("gb/covers/Tiny Tennis (Europe).png"));

    // What path() refuses.
    const auto gbName = systemNamed("gb");
    const auto tennis = keyOf("Tiny Tennis (Europe).gb");
    CHECK(resolver.path(gbName, tennis, false, MediaKind::Manual, MediaPresence{}).error().code == ErrorCode::NotFound);
    CHECK(resolver.path(gbName, rv::catalog::LookupKey{}, false, MediaKind::Cover, MediaPresence{true, ".png"})
              .error()
              .code == ErrorCode::InvalidArgument);
    for (const std::string_view bad : {"", ".", "png", "./x", ".a/b", ".a\\b", ".a:b", "\x01.png"}) {
        INFO(bad);
        CHECK(resolver.path(gbName, tennis, false, MediaKind::Cover, MediaPresence{true, std::string(bad)})
                  .error()
                  .code == ErrorCode::InvalidArgument);
    }
    CHECK(resolver.presence(gbName, rv::catalog::LookupKey{}, false).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("esde/media: names compared as the file system compares them, every extension in ES-DE's order",
          "[esde][media]") {
    const rv::test::TempDir dir("rv-media");
    const fs::path root = dir.file("media");
    // The system's and the kind's folders in another case, a media name in upper case with an upper-case extension.
    write(root / "GB" / "COVERS" / "collection a" / "POCKET PUZZLE (WORLD).PNG");
    // Images: png, then jpg, then webp; other extensions ignored; a folder named like a media file is no media.
    write(root / "GB" / "covers" / "Gem.webp");
    write(root / "GB" / "covers" / "Gem.jpg");
    write(root / "GB" / "covers" / "Webby.webp");
    write(root / "GB" / "covers" / "Animated.gif");
    write(root / "GB" / "covers" / "README");
    write(root / "GB" / "covers" / "Boxed.png" / "inner.txt");
    write(root / "GB" / "covers" / "Boxed.jpg");
    // Videos: mp4, mkv, avi, wmv, mov, webm.
    write(root / "GB" / "videos" / "One.webm");
    write(root / "GB" / "videos" / "One.mp4");
    write(root / "GB" / "videos" / "Two.avi");
    write(root / "GB" / "videos" / "Two.mkv");
    write(root / "GB" / "videos" / "Three.mov");
    write(root / "GB" / "videos" / "Three.wmv");
    write(root / "GB" / "videos" / "Three.webm");
    write(root / "GB" / "videos" / "Four.webm");
    write(root / "GB" / "videos" / "Five.flv");
    write(root / "GB" / "videos" / "Six.MP4");
    // Manuals: pdf only.
    write(root / "GB" / "manuals" / "Seven.PDF");
    write(root / "GB" / "manuals" / "Eight.png");
    // The custom kind is an image kind.
    write(root / "GB" / "custom" / "Nine.webp");
    // Non-ASCII names fold as the file system folds them (é and É), dotted and spaced media names, an empty media name.
    write(root / "GB" / "marquees" / rv::core::pathFromUtf8("café racer (europe).png"));
    write(root / "GB" / "screenshots" / "Game (Europe).dec.png");
    write(root / "GB" / "screenshots" / "Galactic Disk .png");
    write(root / "GB" / "3dboxes" / "Sub" / ".png");
    write(root / "GB" / "physicalmedia" / "a~!@#$%^&()_+-={}[];',`.png");
    // A hidden media file counts: ES-DE's existence test ignores the attribute.
    write(root / "GB" / "fanart" / "Shy.jpg");
    REQUIRE(rv::test::setHidden(root / "GB" / "fanart" / "Shy.jpg", true).has_value());
    // A media path past MAX_PATH.
    std::string deep;
    for (int i = 0; i < 12; ++i) {
        deep += "Folder number " + std::to_string(i) + " with a long name/";
    }
    write(root / "GB" / "titlescreens" / rv::core::pathFromUtf8(deep + "Far Away.png"));
    REQUIRE(rv::core::pathToUtf8(root / rv::core::pathFromUtf8(deep)).size() > 300);

    EsdeMediaResolver resolver(root);
    CHECK(mediaOf(resolver, "gb", "Collection A/Pocket Puzzle (World).gb") == withMedia({{MediaKind::Cover, ".PNG"}}));
    CHECK(mediaOf(resolver, "gb", "Gem.gb") == withMedia({{MediaKind::Cover, ".jpg"}}));
    CHECK(mediaOf(resolver, "gb", "Webby.gb") == withMedia({{MediaKind::Cover, ".webp"}}));
    CHECK(mediaOf(resolver, "gb", "Animated.gb") == EntryMedia{});
    CHECK(mediaOf(resolver, "gb", "README", true) == EntryMedia{});
    CHECK(mediaOf(resolver, "gb", "Boxed.gb") == withMedia({{MediaKind::Cover, ".jpg"}}));
    CHECK(mediaOf(resolver, "gb", "One.zip") == withMedia({{MediaKind::Video, ".mp4"}}));
    CHECK(mediaOf(resolver, "gb", "Two.zip") == withMedia({{MediaKind::Video, ".mkv"}}));
    CHECK(mediaOf(resolver, "gb", "Three.zip") == withMedia({{MediaKind::Video, ".wmv"}}));
    CHECK(mediaOf(resolver, "gb", "Four.zip") == withMedia({{MediaKind::Video, ".webm"}}));
    CHECK(mediaOf(resolver, "gb", "Five.zip") == EntryMedia{});
    CHECK(mediaOf(resolver, "gb", "Six.zip") == withMedia({{MediaKind::Video, ".MP4"}}));
    CHECK(mediaOf(resolver, "gb", "Seven.gb") == withMedia({{MediaKind::Manual, ".PDF"}}));
    CHECK(mediaOf(resolver, "gb", "Eight.gb") == EntryMedia{});
    CHECK(mediaOf(resolver, "gb", "Nine.gb") == withMedia({{MediaKind::Custom, ".webp"}}));
    CHECK(mediaOf(resolver, "gb", "CAFÉ RACER (EUROPE).GB") == withMedia({{MediaKind::Marquee, ".png"}}));
    CHECK(mediaOf(resolver, "gb", "Game (Europe).dec.iso") == withMedia({{MediaKind::Screenshot, ".png"}}));
    CHECK(mediaOf(resolver, "gb", "Galactic Disk .atr") == withMedia({{MediaKind::Screenshot, ".png"}}));
    CHECK(mediaOf(resolver, "gb", "Galactic Disk.atr") == EntryMedia{}); // never trimmed
    CHECK(mediaOf(resolver, "gb", "Sub/.gb") == withMedia({{MediaKind::Box3d, ".png"}}));
    CHECK(mediaOf(resolver, "gb", "A~!@#$%^&()_+-={}[];',`.gb") == withMedia({{MediaKind::PhysicalMedia, ".png"}}));
    CHECK(mediaOf(resolver, "gb", "Shy.gb") == withMedia({{MediaKind::FanArt, ".jpg"}}));
    CHECK(mediaOf(resolver, "gb", deep + "Far Away.gb") == withMedia({{MediaKind::TitleScreen, ".png"}}));

    const rv::esde::SystemMediaListing& gb = listingOf(resolver, "gb");
    CHECK(gb.folderName == "GB");
    CHECK(gb[MediaKind::Cover].folderName == "COVERS");
    CHECK(gb[MediaKind::Cover].media == 4); // Pocket Puzzle, Gem, Webby, Boxed
    CHECK(gb[MediaKind::Cover].shadowedFiles == 1);
    CHECK(gb[MediaKind::Cover].ignored == std::vector<rv::esde::IgnoredExtension>{{"", 1}, {".gif", 1}, {".txt", 1}});
    CHECK(gb[MediaKind::Video].media == 5);
    CHECK(gb[MediaKind::Video].shadowedFiles == 4);
    CHECK(gb[MediaKind::Video].ignored == std::vector<rv::esde::IgnoredExtension>{{".flv", 1}});
    CHECK(gb[MediaKind::Manual].ignored == std::vector<rv::esde::IgnoredExtension>{{".png", 1}});
    CHECK(hasIssue(gb, rv::esde::kMediaIssueIgnoredExtension,
                   "ES-DE reads only .mp4, .mkv, .avi, .wmv, .mov and .webm there"));
    CHECK(hasIssue(gb, rv::esde::kMediaIssueIgnoredExtension, "without an extension"));

    // The path keeps ES-DE's names (the system's, the kind's), the entry's media name and the extension as found.
    const auto path = resolver.path(systemNamed("gb"), keyOf("Collection A/Pocket Puzzle (World).gb"), false,
                                    MediaKind::Cover, MediaPresence{true, ".PNG"});
    REQUIRE(path.has_value());
    CHECK(*path == (root / "gb" / "covers" / "Collection A" / "Pocket Puzzle (World).PNG").make_preferred());
    CHECK(fs::exists(rv::core::extendedLengthPath(*path)));
}

TEST_CASE("esde/media: the listings stay until refresh(), then the folders are read again", "[esde][media]") {
    const rv::test::TempDir dir("rv-media-refresh");
    const fs::path root = dir.file("media");
    write(root / "gb" / "covers" / "First.png");
    EsdeMediaResolver resolver(root);
    CHECK(mediaOf(resolver, "gb", "First.gb") == withMedia({{MediaKind::Cover, ".png"}}));
    CHECK(mediaOf(resolver, "gb", "Later.gb") == EntryMedia{});
    CHECK(resolver.stats().systemsListed == 1);
    CHECK(resolver.stats().files == 1);

    write(root / "gb" / "covers" / "Later.png");
    write(root / "snes" / "videos" / "Other.mp4");
    CHECK(mediaOf(resolver, "gb", "Later.gb") == EntryMedia{}); // the listing of gb is kept
    // So is the media root's: the system folder made since is not seen yet.
    CHECK(mediaOf(resolver, "snes", "Other.sfc") == EntryMedia{});
    CHECK(resolver.stats().systemsListed == 2);
    resolver.refresh();
    CHECK(resolver.stats() == rv::esde::MediaListingStats{});
    CHECK(mediaOf(resolver, "gb", "Later.gb") == withMedia({{MediaKind::Cover, ".png"}}));
    CHECK(resolver.stats().systemsListed == 1);
    CHECK(resolver.stats().files == 2);
    CHECK(mediaOf(resolver, "snes", "Other.sfc") == withMedia({{MediaKind::Video, ".mp4"}}));
}

TEST_CASE("esde/media: an empty media root has no media; one that cannot be read is Unavailable", "[esde][media]") {
    const rv::test::TempDir dir("rv-media-roots");
    const auto gb = systemNamed("gb");
    const auto key = keyOf("Tiny Tennis (Europe).gb");

    // Empty: no system folder, no media, no error.
    fs::create_directories(dir.file("empty"));
    EsdeMediaResolver empty(dir.file("empty"));
    CHECK(empty.checkRoot().has_value());
    CHECK(mediaOf(empty, "gb", "Tiny Tennis (Europe).gb") == EntryMedia{});
    CHECK(listingOf(empty, "gb").state == MediaFolderState::Missing);
    CHECK(listingOf(empty, "gb").issues.empty());

    // A system folder without kind folders, and kind folders that are files: no media either.
    write(dir.file("bare/gb/covers"), "a file, not a folder");
    fs::create_directories(dir.file("bare/snes"));
    EsdeMediaResolver bare(dir.file("bare"));
    CHECK(mediaOf(bare, "gb", "Tiny Tennis (Europe).gb") == EntryMedia{});
    CHECK(listingOf(bare, "gb").state == MediaFolderState::Listed);
    CHECK(listingOf(bare, "gb")[MediaKind::Cover].state == MediaFolderState::Missing);
    CHECK(mediaOf(bare, "snes", "Alpha Quest (USA).sfc") == EntryMedia{});

    const auto checkUnavailable = [&](EsdeMediaResolver& resolver, std::string_view part) {
        const Status root = resolver.checkRoot();
        REQUIRE_FALSE(root.has_value());
        CHECK(root.error().code == ErrorCode::Unavailable);
        INFO(root.error().toString());
        CHECK(root.error().message.find(part) != std::string::npos);
        const auto media = resolver.presence(gb, key, false);
        REQUIRE_FALSE(media.has_value());
        CHECK(media.error().code == ErrorCode::Unavailable);
        CHECK(resolver.listing(gb).error().code == ErrorCode::Unavailable);
    };

    // Missing.
    EsdeMediaResolver missing(dir.file("unplugged/media"));
    checkUnavailable(missing, "does not exist");
    // Paths are derived without looking at the disk.
    CHECK(missing.path(gb, key, false, MediaKind::Cover, MediaPresence{true, ".png"}).has_value());

    // A file.
    write(dir.file("file-root"), "not a folder");
    EsdeMediaResolver file(dir.file("file-root"));
    checkUnavailable(file, "not a folder");

    // A drive letter with no drive behind it: unreachable.
    if (const wchar_t letter = unusedDriveLetter(); letter != 0) {
        EsdeMediaResolver unmapped(fs::path(std::wstring{letter, L':', L'\\'}) / "media");
        checkUnavailable(unmapped, "cannot be read");
    }

    // A root resolveMediaRoot() could not resolve.
    rv::esde::ResolvedRoot unresolved;
    unresolved.source = rv::esde::RootSource::Setting;
    unresolved.configured = "%ESPATH%/media";
    unresolved.issues.push_back(rv::catalog::Issue{rv::catalog::IssueSeverity::Error, "root-unresolved", "", "",
                                                   "%ESPATH% names no known install folder"});
    EsdeMediaResolver unknown(unresolved);
    CHECK_FALSE(unknown.mediaRoot().has_value());
    checkUnavailable(unknown, "%ESPATH% names no known install folder");
    CHECK(unknown.path(gb, key, false, MediaKind::Cover, MediaPresence{true, ".png"}).error().code ==
          ErrorCode::Unavailable);

    // From a resolved root.
    rv::esde::ResolvedRoot resolved;
    resolved.path = dir.file("empty");
    EsdeMediaResolver fromRoot(resolved);
    CHECK(fromRoot.mediaRoot() == resolved.path);
    CHECK(fromRoot.checkRoot().has_value());
}

TEST_CASE("esde/media: folders that cannot be listed make the media they would hold unknown", "[esde][media]") {
    const rv::test::TempDir dir("rv-media-denied");
    const fs::path root = dir.file("media");
    write(root / "gb" / "covers" / "Open.png");
    write(root / "gb" / "covers" / "Locked" / "Inside.png");
    write(root / "gb" / "videos" / "Open.mp4");
    write(root / "snes" / "covers" / "Open.png");
    write(root / "snes" / "videos" / "Open.mp4");
    write(root / "psx" / "covers" / "Open.png");

    {
        // A folder below a kind folder: the entries below it are unknown, the others known.
        auto denied = rv::test::ListingDenied::apply(root / "gb" / "covers" / "Locked");
        REQUIRE(denied.has_value());
        EsdeMediaResolver resolver(root);
        CHECK(mediaOf(resolver, "gb", "Open.gb") ==
              withMedia({{MediaKind::Cover, ".png"}, {MediaKind::Video, ".mp4"}}));
        const auto inside = resolver.presence(systemNamed("gb"), keyOf("Locked/Inside.gb"), false);
        REQUIRE_FALSE(inside.has_value());
        CHECK(inside.error().code == ErrorCode::Unavailable);
        CHECK(mediaOf(resolver, "gb", "Locked", true) == EntryMedia{}); // the folder's own media lie beside it
        const rv::esde::SystemMediaListing& gb = listingOf(resolver, "gb");
        CHECK(gb[MediaKind::Cover].state == MediaFolderState::Listed);
        CHECK(gb[MediaKind::Cover].unlistedFolders == std::vector<std::string>{"Locked"});
        CHECK(hasIssue(gb, rv::esde::kMediaIssueFolderUnlisted, "\"Locked\" of gb/covers"));
    }
    {
        // A kind folder: every entry of the system is unknown.
        auto denied = rv::test::ListingDenied::apply(root / "snes" / "videos");
        REQUIRE(denied.has_value());
        EsdeMediaResolver resolver(root);
        const auto media = resolver.presence(systemNamed("snes"), keyOf("Open.sfc"), false);
        REQUIRE_FALSE(media.has_value());
        CHECK(media.error().code == ErrorCode::Unavailable);
        CHECK(media.error().message.find("snes/videos") != std::string::npos);
        const rv::esde::SystemMediaListing& snes = listingOf(resolver, "snes");
        CHECK(snes[MediaKind::Video].state == MediaFolderState::Unavailable);
        CHECK(snes[MediaKind::Cover].state == MediaFolderState::Listed);
        CHECK(hasIssue(snes, rv::esde::kMediaIssueFolderUnlisted, "snes/videos"));
        // Other systems are not concerned.
        CHECK(mediaOf(resolver, "psx", "Open.cue") == withMedia({{MediaKind::Cover, ".png"}}));
    }
    {
        // A system folder.
        auto denied = rv::test::ListingDenied::apply(root / "psx");
        REQUIRE(denied.has_value());
        EsdeMediaResolver resolver(root);
        const auto media = resolver.presence(systemNamed("psx"), keyOf("Open.cue"), false);
        REQUIRE_FALSE(media.has_value());
        CHECK(media.error().code == ErrorCode::Unavailable);
        const rv::esde::SystemMediaListing& psx = listingOf(resolver, "psx");
        CHECK(psx.state == MediaFolderState::Unavailable);
        CHECK(hasIssue(psx, rv::esde::kMediaIssueFolderUnlisted, "the media folder of psx"));
        CHECK(mediaOf(resolver, "gb", "Open.gb") ==
              withMedia({{MediaKind::Cover, ".png"}, {MediaKind::Video, ".mp4"}}));
    }
}
