// I12 step 2 (ADR-0021 §2): the gallery's pictures, GPU-free (gallery_pictures.hpp): a room's candidates from its
// items, the scene's frames, the choice of a medium per frame, the picture fitted to its frame.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/gallery_pictures.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/gallery.hpp>

namespace {

using rv::app::GalleryCandidate;
using rv::app::GalleryChoice;
using rv::app::GalleryTarget;
using rv::app::RoomGallery;
using rv::app::RoomItemScreen;
using rv::app::RoomItemText;
using rv::world::GalleryKind;

rv::catalog::GameId gameId(std::uint64_t value) {
    return rv::catalog::GameId::fromValue(value).value();
}

rv::world::CollectionLayout layoutOf(std::initializer_list<std::uint64_t> games) {
    rv::world::CollectionLayout layout;
    std::uint32_t pick = 1;
    for (const std::uint64_t game : games) {
        rv::world::CollectionItem item;
        item.game = gameId(game);
        item.pickId = pick++;
        layout.items.push_back(item);
    }
    return layout;
}

/// An item whose game has the media named (relative to the media root), its priority data.
RoomItemText text(std::uint64_t game, const std::string& name, bool favorite, std::uint64_t plays) {
    RoomItemText item;
    item.game = gameId(game);
    item.title = name;
    item.coverPath = "snes/covers/" + name + ".png";
    RoomItemScreen screen;
    screen.sortKey = name;
    screen.favorite = favorite;
    screen.playCount = plays;
    item.screen = screen;
    return item;
}

std::size_t index(GalleryKind kind) {
    return static_cast<std::size_t>(kind);
}

/// A gallery of `count` candidates in priority order, each with every kind's medium.
RoomGallery fullGallery(std::size_t count, std::uint64_t seed) {
    RoomGallery gallery;
    gallery.seed = seed;
    for (std::size_t i = 0; i < count; ++i) {
        GalleryCandidate candidate;
        candidate.game = gameId(100 + i);
        for (const GalleryKind kind : rv::world::kGalleryKinds) {
            candidate.media[index(kind)] =
                std::filesystem::path("m") / (std::string(rv::world::toString(kind)) + std::to_string(i) + ".png");
        }
        gallery.candidates.push_back(candidate);
    }
    return gallery;
}

std::vector<GalleryTarget> targetsOf(std::initializer_list<std::pair<const char*, GalleryKind>> frames) {
    std::vector<GalleryTarget> targets;
    for (const auto& [fixture, kind] : frames) {
        targets.push_back(
            GalleryTarget{"room:snes/dressing:gallery:" + std::string(fixture), "room:snes", fixture, kind});
    }
    return targets;
}

/// A frame of the scene as the scene builder writes it: a poster_frame of `kind`'s picture with its bars.
rv::scene::SceneObject frameObject(const std::string& id, GalleryKind kind) {
    rv::scene::SceneObject object;
    object.id = id;
    object.source = rv::scene::SceneObject::Source::Prefab;
    object.kind = "poster_frame";
    const rv::world::GalleryPictureSize picture = rv::world::galleryPictureSize(kind);
    object.params = nlohmann::json{{"width", static_cast<double>(picture.widthMm + 50) / 1000.0},
                                   {"height", static_cast<double>(picture.heightMm + 50) / 1000.0},
                                   {"frameWidth", 0.025},
                                   {"depth", 0.03}};
    return object;
}

/// An image of `width` x `height`, RGBA, filled with `rgba`.
rv::assets::ImageData filled(int width, int height, std::array<std::uint8_t, 4> rgba) {
    rv::assets::ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        std::copy(rgba.begin(), rgba.end(), image.pixels.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return image;
}

std::array<std::uint8_t, 4> pixel(const rv::assets::ImageData& image, int x, int y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

} // namespace

TEST_CASE("app/gallery_pictures: a room's candidates from its items, by priority, each kind's medium",
          "[app][gallery]") {
    const std::filesystem::path root = std::filesystem::path("E:/media");
    std::vector<RoomItemText> items{text(10, "Amber", false, 3), text(20, "Bramble", true, 0),
                                    text(30, "Cobalt", false, 9), text(40, "Dapple", false, 0)};
    items[0].screen->fanArt = "snes/fanart/Amber.jpg";
    items[0].screen->box3d = "snes/3dboxes/Amber.png";
    items[0].screen->marquee = "snes/marquees/Amber.png";
    items[0].screen->titleScreen = "snes/titlescreens/Amber.png";
    items[1].screen->screenshot = "snes/screenshots/Bramble.png";
    items[1].screen->titleScreen = "snes/titlescreens/Bramble.png";
    items[2].screen->mixImage = "snes/miximages/Cobalt.png";
    items[3].coverPath.clear(); // no medium at all: left out
    const rv::world::CollectionLayout layout = layoutOf({10, 20, 30, 40, 50, 20});
    const auto lookup = [&](const rv::catalog::GameId& game) -> const RoomItemText* {
        for (const RoomItemText& item : items) {
            if (item.game == game) {
                return &item;
            }
        }
        return nullptr; // 50: nothing known of it
    };
    const RoomGallery gallery = rv::app::roomGallery(layout, lookup, root, 77);
    CHECK(gallery.seed == 77);
    // The favourite first, then by plays; the game without media and the unknown one left out, each game once.
    REQUIRE(gallery.candidates.size() == 3);
    CHECK(gallery.candidates[0].game == gameId(20));
    CHECK(gallery.candidates[1].game == gameId(30));
    CHECK(gallery.candidates[2].game == gameId(10));
    const GalleryCandidate& amber = gallery.candidates[2];
    CHECK(amber.media[index(GalleryKind::Poster)].filename() == "Amber.jpg");
    CHECK(amber.media[index(GalleryKind::Portrait)].parent_path().filename() == "3dboxes"); // the 3D box first
    CHECK(amber.media[index(GalleryKind::Lightbox)].parent_path().filename() == "marquees");
    CHECK(amber.media[index(GalleryKind::Frame)].parent_path().filename() == "titlescreens"); // no screenshot
    const GalleryCandidate& bramble = gallery.candidates[0];
    CHECK(bramble.media[index(GalleryKind::Poster)].empty());
    CHECK(bramble.media[index(GalleryKind::Portrait)].parent_path().filename() == "covers"); // the cover instead
    CHECK(bramble.media[index(GalleryKind::Lightbox)].empty());
    CHECK(bramble.media[index(GalleryKind::Frame)].parent_path().filename() == "screenshots");
    CHECK(gallery.candidates[1].media[index(GalleryKind::Frame)].parent_path().filename() == "miximages");
}

TEST_CASE("app/gallery_pictures: the scene's frames and their kinds, never the hoods or the other pictures",
          "[app][gallery]") {
    rv::scene::SceneDescription description;
    description.objects.push_back(frameObject("room:snes/dressing:gallery:fx:w01", GalleryKind::Poster));
    description.objects.push_back(frameObject("room:snes/dressing:gallery:fx:w02", GalleryKind::Portrait));
    description.objects.push_back(frameObject("area:arcade/dressing:gallery:fx:e01", GalleryKind::Lightbox));
    description.objects.push_back(frameObject("dressing:gallery:fx:hw-w", GalleryKind::Frame));
    description.objects.push_back(frameObject("room:snes/dressing:picture:fx:dec-se", GalleryKind::Poster));
    description.objects.push_back(frameObject("room:snes/xdressing:gallery:fx:w03", GalleryKind::Poster));
    rv::scene::SceneObject hood;
    hood.id = "room:snes/dressing:gallery:fx:w01/hood";
    hood.source = rv::scene::SceneObject::Source::Primitive;
    hood.kind = "box";
    description.objects.push_back(hood);
    const std::vector<GalleryTarget> targets = rv::app::galleryTargets(description);
    REQUIRE(targets.size() == 4);
    CHECK(targets[0].room == "room:snes");
    CHECK(targets[0].fixture == "fx:w01");
    CHECK(targets[0].kind == GalleryKind::Poster);
    CHECK(targets[1].kind == GalleryKind::Portrait);
    CHECK(targets[2].room == "area:arcade");
    CHECK(targets[2].kind == GalleryKind::Lightbox);
    CHECK(targets[3].room.empty()); // a scene of one room without prefixes
    CHECK(targets[3].fixture == "fx:hw-w");
    CHECK(targets[3].kind == GalleryKind::Frame);
}

TEST_CASE("app/gallery_pictures: the choice: by the room's seed among the first candidates, a game once, the print "
          "where none fits",
          "[app][gallery]") {
    const std::vector<GalleryTarget> targets =
        targetsOf({{"fx:w01", GalleryKind::Poster}, {"fx:w02", GalleryKind::Portrait}, {"fx:w03", GalleryKind::Frame}});
    const RoomGallery gallery = fullGallery(20, 1234);
    const std::vector<GalleryChoice> choices = rv::app::chooseGalleryPictures(targets, gallery);
    REQUIRE(choices.size() == 3);
    std::set<rv::catalog::GameId> games;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        INFO(i);
        CHECK(choices[i].objectId == targets[i].objectId);
        CHECK(choices[i].kind == targets[i].kind);
        // Among the first twice-as-many-as-frames candidates, the kind's medium of the game chosen.
        const auto at = static_cast<std::size_t>(
            std::find_if(gallery.candidates.begin(), gallery.candidates.end(),
                         [&](const GalleryCandidate& candidate) { return candidate.game == choices[i].game; }) -
            gallery.candidates.begin());
        CHECK(at < 6);
        CHECK(choices[i].media == gallery.candidates[at].media[index(choices[i].kind)]);
        games.insert(choices[i].game);
    }
    CHECK(games.size() == 3); // a game at most once a room
    // The same frames and seed choose the same; another seed may choose otherwise, but always from the first six.
    CHECK(rv::app::chooseGalleryPictures(targets, gallery) == choices);
    bool differs = false;
    for (std::uint64_t seed = 1; seed < 40 && !differs; ++seed) {
        differs = rv::app::chooseGalleryPictures(targets, fullGallery(20, seed)) != choices;
    }
    CHECK(differs);

    // A frame of a kind no candidate has keeps its print; a kind only a later candidate has takes it from beyond the
    // first ones; a game used once is not chosen again.
    RoomGallery sparse = fullGallery(10, 5);
    for (GalleryCandidate& candidate : sparse.candidates) {
        candidate.media[index(GalleryKind::Lightbox)].clear();
        candidate.media[index(GalleryKind::Poster)].clear();
    }
    sparse.candidates[9].media[index(GalleryKind::Poster)] = "m/late.png";
    const std::vector<GalleryTarget> mixed = targetsOf(
        {{"fx:e01", GalleryKind::Lightbox}, {"fx:e02", GalleryKind::Poster}, {"fx:e03", GalleryKind::Poster}});
    const std::vector<GalleryChoice> sparseChoices = rv::app::chooseGalleryPictures(mixed, sparse);
    REQUIRE(sparseChoices.size() == 1);
    CHECK(sparseChoices[0].objectId == mixed[1].objectId);
    CHECK(sparseChoices[0].media == std::filesystem::path("m/late.png"));
    // No candidates: every frame its print.
    CHECK(rv::app::chooseGalleryPictures(targets, RoomGallery{}).empty());
}

TEST_CASE("app/gallery_pictures: a picture fitted inside its frame on a mat of its own colour, never stretched",
          "[app][gallery]") {
    // A wide orange picture in a portrait frame: across the whole width, bands of its darkened colour above and below.
    const auto png = [](const rv::assets::ImageData& image) {
        auto bytes = rv::assets::encodePng(image);
        REQUIRE(bytes.has_value());
        return *bytes;
    };
    const std::vector<std::byte> wide = png(filled(400, 200, {230, 120, 30, 255}));
    auto portrait = rv::app::makeGalleryPicture(wide, GalleryKind::Portrait, "wide.png");
    REQUIRE(portrait.has_value());
    const glm::ivec2 size = rv::app::galleryUploadSize(GalleryKind::Portrait);
    CHECK(portrait->width == size.x);
    CHECK(portrait->height == size.y);
    CHECK(portrait->channels == 4);
    const auto centre = pixel(*portrait, size.x / 2, size.y / 2);
    CHECK(centre[0] == 230);
    CHECK(centre[1] == 120);
    CHECK(centre[2] == 30);
    // The picture's height at the frame's width: 640 x 320, centred; the mat above it the colour darkened.
    const auto mat = pixel(*portrait, size.x / 2, 10);
    CHECK(mat[0] < 140);
    CHECK(mat[0] > mat[2]);
    CHECK(mat[3] == 255);
    CHECK(pixel(*portrait, size.x / 2, size.y / 2 - 150)[0] == 230);
    CHECK(pixel(*portrait, size.x / 2, size.y / 2 - 170) == mat);
    CHECK(pixel(*portrait, 2, size.y / 2)[0] == 230); // across the whole width

    // A screenshot in a frame: on a mat all round, even at the frame's own aspect.
    const std::vector<std::byte> shot = png(filled(512, 384, {40, 160, 200, 255}));
    auto framed = rv::app::makeGalleryPicture(shot, GalleryKind::Frame, "shot.png");
    REQUIRE(framed.has_value());
    const glm::ivec2 frameSize = rv::app::galleryUploadSize(GalleryKind::Frame);
    CHECK(pixel(*framed, 4, frameSize.y / 2) != pixel(*framed, frameSize.x / 2, frameSize.y / 2));
    CHECK(pixel(*framed, frameSize.x / 2, 4) != pixel(*framed, frameSize.x / 2, frameSize.y / 2));

    // A marquee's transparent ground on a nearly black mat, its lettering kept.
    rv::assets::ImageData marquee = filled(300, 100, {255, 255, 255, 0});
    for (int y = 40; y < 60; ++y) {
        for (int x = 100; x < 200; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * 300 + static_cast<std::size_t>(x)) * 4;
            marquee.pixels[at] = 250;
            marquee.pixels[at + 1] = 200;
            marquee.pixels[at + 2] = 40;
            marquee.pixels[at + 3] = 255;
        }
    }
    auto lightbox = rv::app::makeGalleryPicture(png(marquee), GalleryKind::Lightbox, "marquee.png");
    REQUIRE(lightbox.has_value());
    const glm::ivec2 boxSize = rv::app::galleryUploadSize(GalleryKind::Lightbox);
    const auto ground = pixel(*lightbox, boxSize.x / 2, 8);
    CHECK(ground[0] < 40);
    CHECK(ground[3] == 255);
    const auto letter = pixel(*lightbox, boxSize.x / 2, boxSize.y / 2);
    CHECK(letter[0] > 200);

    // Not an image: refused.
    const std::vector<std::byte> junk(64, std::byte{7});
    CHECK_FALSE(rv::app::makeGalleryPicture(junk, GalleryKind::Poster, "junk.png").has_value());
}

TEST_CASE("app/gallery_pictures: the upload sizes are their kinds' shapes, in whole blocks", "[app][gallery]") {
    for (const GalleryKind kind : rv::world::kGalleryKinds) {
        INFO(rv::world::toString(kind));
        const glm::ivec2 size = rv::app::galleryUploadSize(kind);
        CHECK(size.x % 4 == 0);
        CHECK(size.y % 4 == 0);
        CHECK(rv::world::galleryKindOfAspect(static_cast<double>(size.x) / static_cast<double>(size.y)) == kind);
    }
}
