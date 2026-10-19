// What the media viewer and Informazioni show of a game (I3 step 14b; rv/app/game_screens.hpp): the viewer's pages
// from the catalog's stored media in the viewer's order, the missing kinds and the kinds it does not page skipped, the
// paths as ES-DE names them; the media root from library.json; a page's picture read, decoded and reduced, its
// failures (missing, truncated, not an image); the other versions of a title from a room's plan, with their titles and
// region tags; Informazioni's content from the catalog's row.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rv/app/game_screens.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/core/file_io.hpp>
#include <rv/esde/discovery.hpp>

#include "support/temp_dir.hpp"

using rv::app::MediaPageFile;
using rv::catalog::GameId;
using rv::catalog::MediaKind;

namespace {

GameId game(std::uint64_t value) {
    return GameId::fromValue(value).value();
}

rv::catalog_db::GameDetails detailsOf(std::uint64_t id, std::string_view relativePath, std::string title) {
    rv::catalog_db::GameDetails details;
    details.indexed.id = game(id);
    rv::catalog::CatalogEntry& entry = details.indexed.entry;
    entry.system = rv::catalog::SystemName::parse("snes").value();
    entry.key = rv::catalog::makeLookupKey(relativePath).value();
    entry.title = std::move(title);
    entry.file = rv::catalog::FileFacts{};
    return details;
}

void present(rv::catalog_db::GameDetails& details, MediaKind kind, std::string extension) {
    details.indexed.media[kind] = rv::catalog::MediaPresence{true, std::move(extension)};
}

rv::assets::ImageData picture(int width, int height) {
    rv::assets::ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 3;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
    for (std::size_t index = 0; index < image.pixels.size(); ++index) {
        image.pixels[index] = static_cast<std::uint8_t>((index * 37u) % 251u);
    }
    return image;
}

void writeBytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    REQUIRE(rv::core::writeFileAtomic(path, bytes).has_value());
}

} // namespace

TEST_CASE("game screens: the viewer's pages follow its order, the missing and the unpaged kinds skipped",
          "[app][game_screens]") {
    rv::catalog_db::GameDetails details = detailsOf(1, "Amber Lantern (USA).sfc", "Amber Lantern");
    present(details, MediaKind::FanArt, ".jpg");
    present(details, MediaKind::Cover, ".png");
    present(details, MediaKind::Screenshot, ".jpg");
    present(details, MediaKind::Box3d, ".png");
    present(details, MediaKind::MixImage, ".png"); // not a page
    present(details, MediaKind::Video, ".mp4");    // the viewer's videos are I5's
    present(details, MediaKind::Marquee, "png");   // no file extension: no path, as ES-DE's resolver refuses it
    const std::vector<MediaPageFile> pages = rv::app::viewerPagesOf(details);
    const std::vector<MediaPageFile> expected{
        {"covers", "snes/covers/Amber Lantern (USA).png"},
        {"3dboxes", "snes/3dboxes/Amber Lantern (USA).png"},
        {"screenshots", "snes/screenshots/Amber Lantern (USA).jpg"},
        {"fanart", "snes/fanart/Amber Lantern (USA).jpg"},
    };
    CHECK(pages == expected);

    // A game in a folder of the system, a game without media, an entry without a key.
    rv::catalog_db::GameDetails nested = detailsOf(2, "Collection A/Thistle Trellis (USA).sfc", "Thistle Trellis");
    present(nested, MediaKind::Screenshot, ".jpg");
    CHECK(rv::app::viewerPagesOf(nested) ==
          std::vector<MediaPageFile>{{"screenshots", "snes/screenshots/Collection A/Thistle Trellis (USA).jpg"}});
    CHECK(rv::app::viewerPagesOf(detailsOf(3, "Bare (USA).sfc", "Bare")).empty());
    rv::catalog_db::GameDetails keyless;
    present(keyless, MediaKind::Cover, ".png");
    CHECK(rv::app::viewerPagesOf(keyless).empty());
}

TEST_CASE("game screens: a page's picture is read, decoded and reduced; what cannot be shown says why",
          "[app][game_screens]") {
    rv::test::TempDir dir("rv-game-screens");
    const std::filesystem::path media = dir.path() / "media";
    auto png = rv::assets::encodePng(picture(100, 50));
    REQUIRE(png.has_value());
    writeBytes(media / "snes" / "covers" / "Amber Lantern (USA).png", *png);
    writeBytes(media / "snes" / "screenshots" / "Bramble Burrow Rally (USA).jpg",
               std::span(png->data(), png->size() / 2)); // a truncated file
    const std::string text = "not an image";
    writeBytes(media / "snes" / "marquees" / "Text (USA).png", std::as_bytes(std::span(text.data(), text.size())));

    auto whole = rv::app::viewerPicture(media, "snes/covers/Amber Lantern (USA).png");
    REQUIRE(whole.has_value());
    CHECK(whole->width == 100);
    CHECK(whole->height == 50);
    CHECK(whole->channels == 4);
    auto reduced = rv::app::viewerPicture(media, "snes/covers/Amber Lantern (USA).png", 40);
    REQUIRE(reduced.has_value());
    CHECK(reduced->width == 40);
    CHECK(reduced->height == 20);

    auto truncated = rv::app::viewerPicture(media, "snes/screenshots/Bramble Burrow Rally (USA).jpg");
    REQUIRE_FALSE(truncated.has_value());
    CHECK(truncated.error().code == rv::ErrorCode::ParseError);
    auto notImage = rv::app::viewerPicture(media, "snes/marquees/Text (USA).png");
    REQUIRE_FALSE(notImage.has_value());
    CHECK(notImage.error().code == rv::ErrorCode::Unsupported);
    auto missing = rv::app::viewerPicture(media, "snes/fanart/Amber Lantern (USA).jpg");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);
}

TEST_CASE("game screens: the media root is library.json's override, else ES-DE's", "[app][game_screens]") {
    rv::test::TempDir dir("rv-game-screens");
    rv::catalog::LibraryProfile profile;
    profile.id = rv::catalog::LibraryId::parse("esde").value();
    profile.esdeHome = dir.path() / "ES-DE";
    profile.mediaRoot = dir.path() / "media";
    const std::filesystem::path file = dir.path() / "config" / "library.json";
    std::filesystem::create_directories(file.parent_path());
    REQUIRE(rv::core::writeTextFileAtomic(file, rv::esde::libraryFileText(profile)).has_value());
    auto root = rv::app::viewerMediaRoot(file);
    REQUIRE(root.has_value());
    CHECK(*root == dir.path() / "media");

    // Without the override: ES-DE's default under its home (no settings file: ES-DE's defaults).
    profile.mediaRoot.reset();
    REQUIRE(rv::core::writeTextFileAtomic(file, rv::esde::libraryFileText(profile)).has_value());
    auto fallback = rv::app::viewerMediaRoot(file);
    REQUIRE(fallback.has_value());
    CHECK(fallback->parent_path() == dir.path() / "ES-DE");

    CHECK_FALSE(rv::app::viewerMediaRoot(dir.path() / "config" / "missing.json").has_value());
}

TEST_CASE("game screens: the other versions of a title from a room's plan, with their titles and regions",
          "[app][game_screens]") {
    rv::world::WorldPlan plan;
    using rv::world::UnplacedGame;
    using rv::world::UnplacedReason;
    plan.unplaced.push_back(UnplacedGame{game(12), {}, UnplacedReason::DuplicateTitle, game(10)});
    plan.unplaced.push_back(UnplacedGame{game(11), {}, UnplacedReason::DuplicateTitle, game(10)});
    plan.unplaced.push_back(UnplacedGame{game(21), {}, UnplacedReason::DuplicateTitle, game(20)});
    plan.unplaced.push_back(UnplacedGame{game(30), {}, UnplacedReason::NoCover, {}});
    const rv::app::VersionGroups groups = rv::app::VersionGroups::of(plan);
    CHECK_FALSE(groups.empty());
    CHECK(groups.othersOf(game(10)) == std::vector<GameId>{game(11), game(12)});
    CHECK(groups.othersOf(game(11)) == std::vector<GameId>{game(10), game(12)});
    CHECK(groups.othersOf(game(21)) == std::vector<GameId>{game(20)});
    CHECK(groups.othersOf(game(20)) == std::vector<GameId>{game(21)});
    CHECK(groups.othersOf(game(30)).empty());
    CHECK(groups.othersOf(game(99)).empty());
    CHECK(rv::app::VersionGroups{}.othersOf(game(10)).empty());

    const rv::ui::OtherVersion europe =
        rv::app::otherVersionOf(game(11), detailsOf(11, "Cobalt Compass (Europe).sfc", "Cobalt Compass"));
    CHECK(europe == rv::ui::OtherVersion{game(11).toString(), "Cobalt Compass", "Europe"});
    const rv::ui::OtherVersion untagged =
        rv::app::otherVersionOf(game(12), detailsOf(12, "Cobalt Compass.sfc", "Cobalt Compass"));
    CHECK(untagged.region.empty());
    CHECK(rv::app::otherVersionOf(game(13), std::nullopt) == rv::ui::OtherVersion{game(13).toString(), {}, {}});
}

TEST_CASE("game screens: Informazioni's content from the catalog's row, the versions kept to its list",
          "[app][game_screens]") {
    rv::ui::CatalogEntry entry;
    entry.id = game(1).toString();
    entry.title = "Amber Lantern";
    entry.platform = "Lumen 16 Home System";
    entry.year = 1994;
    entry.developer = "Paper Owl";
    entry.publisher = "Quiet Harbor";
    entry.genre = "Avventura";
    entry.players = "1-2";
    entry.description = "Una lanterna.\nDue righe.";
    std::vector<rv::ui::OtherVersion> versions;
    for (std::uint64_t index = 0; index < 15; ++index) {
        versions.push_back(rv::ui::OtherVersion{game(100 + index).toString(), "Amber Lantern", "Europe"});
    }
    const rv::ui::GameInfoContent content = rv::app::gameInfoOf(entry.id, "room title", entry, versions);
    CHECK(content.details);
    CHECK(content.gameId == entry.id);
    CHECK(content.title == "Amber Lantern");
    CHECK(content.platform == "Lumen 16 Home System");
    CHECK(content.year == 1994);
    CHECK(content.developer == "Paper Owl");
    CHECK(content.publisher == "Quiet Harbor");
    CHECK(content.genre == "Avventura");
    CHECK(content.players == "1-2");
    CHECK(content.description == "Una lanterna.\nDue righe.");
    CHECK(content.versionCount == 15);
    CHECK(content.versions.size() == rv::ui::game_info::kMaxVersions);

    // Before the catalog answers: the room's title alone.
    const rv::ui::GameInfoContent early = rv::app::gameInfoOf(entry.id, "room title", std::nullopt, {});
    CHECK_FALSE(early.details);
    CHECK(early.title == "room title");
    CHECK(early.description.empty());
    CHECK(early.versionCount == 0);
}
