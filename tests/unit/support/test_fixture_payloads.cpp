// The media payloads of files.json (I3 step 1c, support/fixture_payloads.hpp) and its generated collections
// (support/fixture_collection.hpp): pictures that decode with assets::decodeImage (or fail as asked), clips that play
// with their frame counter (or fail as asked), a collection's rules, and the materializer's checks of both.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/media/test_video.hpp>
#include <rv/media/video_decoder.hpp>

#include "../media/media_test_support.hpp"
#include "support/fixture.hpp"
#include "support/fixture_collection.hpp"
#include "support/fixture_payloads.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::media::DecodeFailure;
using rv::media::DecoderState;
using rv::test::ClipDamage;
using rv::test::ClipPayload;
using rv::test::PictureDamage;
using rv::test::PictureFormat;
using rv::test::PicturePayload;
using rv::test::PictureShape;

namespace {

rv::Expected<rv::assets::ImageData> decode(const std::string& bytes) {
    return rv::assets::decodeImage(std::as_bytes(std::span(bytes.data(), bytes.size())), 0, "payload");
}

PicturePayload picture(std::string title, std::uint32_t seed) {
    PicturePayload payload;
    payload.title = std::move(title);
    payload.seed = seed;
    return payload;
}

std::string bytesOf(const PicturePayload& payload) {
    auto bytes = rv::test::pictureBytes(payload);
    INFO((bytes ? "encoded" : bytes.error().toString()));
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

rv::Expected<PicturePayload> parsePicture(std::string_view json, std::string_view extension = ".png") {
    return rv::test::parsePicturePayload(nlohmann::json::parse(json), extension, "covers/A.png");
}

rv::Expected<ClipPayload> parseClip(std::string_view json, std::string_view extension = ".mp4") {
    return rv::test::parseClipPayload(nlohmann::json::parse(json), extension, "videos/A.mp4");
}

DecodeFailure endOf(const fs::path& clip) {
    rv::media::DecoderSettings settings;
    settings.loop = false;
    auto decoder = rv::test::startDecoder(clip, settings);
    (void)rv::test::collectFrames(*decoder, 1000);
    REQUIRE(rv::test::waitUntil(
        [&] { return decoder->state() == DecoderState::Failed || decoder->state() == DecoderState::Ended; }));
    return decoder->failure();
}

} // namespace

TEST_CASE("support/fixture_payloads: covers decode as encoded, landscape, portrait, PNG and JPEG",
          "[support][fixture][payloads]") {
    PicturePayload landscape = picture("Amber Lantern", 7);
    const std::string png = bytesOf(landscape);
    const auto decoded = decode(png);
    REQUIRE(decoded.has_value());
    CHECK(decoded->width == 680);
    CHECK(decoded->height == 497);
    CHECK(decoded->channels == 3);                                      // opaque, as scraped covers are
    CHECK(decoded->pixels == rv::test::pictureImage(landscape).pixels); // PNG is lossless
    CHECK(bytesOf(landscape) == png);                                   // deterministic

    // Another seed or title gives another picture.
    CHECK(bytesOf(picture("Amber Lantern", 8)) != png);
    CHECK(bytesOf(picture("Amber Lanterns", 7)) != png);

    PicturePayload portrait = landscape;
    portrait.width = 497;
    portrait.height = 680;
    portrait.format = PictureFormat::Jpeg;
    const auto jpeg = decode(bytesOf(portrait));
    REQUIRE(jpeg.has_value());
    CHECK(jpeg->width == 497);
    CHECK(jpeg->height == 680);
    CHECK(jpeg->channels == 3);
    CHECK(bytesOf(portrait).starts_with("\xFF\xD8"));

    PicturePayload tiny = landscape;
    tiny.width = 16;
    tiny.height = 16;
    CHECK(decode(bytesOf(tiny))->width == 16);
    tiny.width = 4097;
    CHECK_FALSE(rv::test::pictureBytes(tiny).has_value());
}

TEST_CASE("support/fixture_payloads: the quick PNG writer gives back every pixel", "[support][fixture][payloads]") {
    // Grey, grey with alpha, RGB and RGBA; flat runs, repeated rows, noise and a run longer than one match.
    for (int channels = 1; channels <= 4; ++channels) {
        for (const auto [width, height] : {std::pair{1, 1}, std::pair{3, 2}, std::pair{301, 7}, std::pair{64, 64}}) {
            rv::assets::ImageData image;
            image.width = width;
            image.height = height;
            image.channels = channels;
            image.pixels.resize(image.rowBytes() * static_cast<std::size_t>(height));
            std::uint32_t state = 12345u + static_cast<std::uint32_t>(channels * 7 + width);
            for (std::size_t i = 0; i < image.pixels.size(); ++i) {
                state = state * 1664525u + 1013904223u;
                const bool noise = (i / 97) % 3 == 0;
                image.pixels[i] =
                    noise ? static_cast<std::uint8_t>(state >> 24) : static_cast<std::uint8_t>(i % 5 * 40);
            }
            INFO(channels << " channels, " << width << "x" << height);
            const std::string png = rv::test::encodeQuickPng(image);
            const auto decoded = decode(png);
            REQUIRE(decoded.has_value());
            CHECK(decoded->channels == channels);
            CHECK(decoded->pixels == image.pixels);
            CHECK(rv::test::encodeQuickPng(image) == png);
        }
    }
    // A cover through it decodes to the same pixels as through stb_image_write.
    PicturePayload payload = picture("Velvet Vineyard", 21);
    const std::string stb = bytesOf(payload);
    payload.quickPng = true;
    const std::string quick = bytesOf(payload);
    CHECK(quick != stb);
    CHECK(decode(quick)->pixels == decode(stb)->pixels);
}

TEST_CASE("support/fixture_payloads: a cartridge stands on a transparent background", "[support][fixture][payloads]") {
    PicturePayload cartridge = picture("Cobalt Compass", 3);
    cartridge.width = 600;
    cartridge.height = 520;
    cartridge.shape = PictureShape::Cartridge;
    const auto decoded = decode(bytesOf(cartridge));
    REQUIRE(decoded.has_value());
    CHECK(decoded->channels == 4);
    const auto alphaAt = [&](int x, int y) {
        return decoded->pixels[(static_cast<std::size_t>(y) * 600u + static_cast<std::size_t>(x)) * 4u + 3u];
    };
    CHECK(alphaAt(0, 0) == 0);       // the corner, outside the shell
    CHECK(alphaAt(599, 519) == 0);   // and the other
    CHECK(alphaAt(300, 260) == 255); // the label
    CHECK(alphaAt(300, 40) == 255);  // the grip

    cartridge.format = PictureFormat::Jpeg;
    CHECK_FALSE(rv::test::pictureBytes(cartridge).has_value());
}

TEST_CASE("support/fixture_payloads: damaged covers fail to decode as asked", "[support][fixture][payloads]") {
    PicturePayload payload = picture("Bellows Brigade", 44);
    const std::string whole = bytesOf(payload);

    payload.damage = PictureDamage::Truncated;
    const std::string truncated = bytesOf(payload);
    CHECK(truncated.size() == whole.size() / 2);
    CHECK(whole.starts_with(truncated));
    const auto cut = decode(truncated);
    REQUIRE_FALSE(cut.has_value());
    CHECK(cut.error().code == rv::ErrorCode::ParseError);

    payload.format = PictureFormat::Jpeg;
    const auto cutJpeg = decode(bytesOf(payload));
    // stb_image reads what a truncated baseline JPEG holds, or refuses it: never an image of the whole picture.
    if (cutJpeg) {
        CHECK(cutJpeg->width == 680);
    } else {
        CHECK(cutJpeg.error().code == rv::ErrorCode::ParseError);
    }

    payload.damage = PictureDamage::NotAnImage;
    const std::string text = bytesOf(payload);
    CHECK(text.find("Bellows Brigade") != std::string::npos);
    const auto notImage = decode(text);
    REQUIRE_FALSE(notImage.has_value());
    CHECK(notImage.error().code == rv::ErrorCode::Unsupported);
}

TEST_CASE("support/fixture_payloads: files.json's cover object", "[support][fixture][payloads]") {
    const auto plain = parsePicture(R"({"title": "A", "seed": 1})");
    REQUIRE(plain.has_value());
    CHECK(plain->width == 680);
    CHECK(plain->height == 497);
    CHECK(plain->format == PictureFormat::Png);
    CHECK(plain->shape == PictureShape::Box);
    CHECK(plain->damage == PictureDamage::None);

    CHECK(parsePicture(R"({"title": "A", "seed": 1})", ".JPG")->format == PictureFormat::Jpeg);
    CHECK(parsePicture(R"({"title": "A", "seed": 1, "format": "jpeg"})", ".png")->format == PictureFormat::Jpeg);
    const auto full = parsePicture(
        R"({"title": "A", "seed": 1, "width": 64, "height": 32, "shape": "cartridge", "damage": "truncated"})");
    REQUIRE(full.has_value());
    CHECK(full->width == 64);
    CHECK(full->height == 32);
    CHECK(full->shape == PictureShape::Cartridge);
    CHECK(full->damage == PictureDamage::Truncated);
    CHECK(parsePicture(R"({"title": "A", "seed": 1, "damage": "not-an-image"})")->damage == PictureDamage::NotAnImage);

    for (const std::string_view bad : {
             R"({"seed": 1})",                                                       // no title
             R"({"title": "A"})",                                                    // no seed
             R"({"title": "A", "seed": -1})",                                        // a negative seed
             R"({"title": "A", "seed": 1, "width": 0})",                             // too small
             R"({"title": "A", "seed": 1, "height": 4097})",                         // too large
             R"({"title": "A", "seed": 1, "format": "gif"})",                        // no such format
             R"({"title": "A", "seed": 1, "shape": "disc"})",                        // no such shape
             R"({"title": "A", "seed": 1, "damage": "burnt"})",                      // no such damage
             R"({"title": "A", "seed": 1, "colour": "red"})",                        // an unknown key
             R"({"title": "A", "seed": 1, "shape": "cartridge", "format": "jpeg"})", // no transparency in a JPEG
             R"("A")",                                                               // not an object
         }) {
        INFO(bad);
        const auto parsed = parsePicture(bad);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == rv::ErrorCode::ValidationFailed);
        CHECK(parsed.error().message.starts_with("covers/A.png: "));
    }
    // Neither .png nor .jpg, and no format.
    CHECK_FALSE(parsePicture(R"({"title": "A", "seed": 1})", ".webp").has_value());
}

TEST_CASE("support/fixture_payloads: a clip plays with its frame counter", "[support][fixture][payloads]") {
    const rv::test::MediaTestDir dir;
    auto payload = parseClip(R"({"seed": 5, "seconds": 1, "frameRate": 10})");
    REQUIRE(payload.has_value());
    CHECK(payload->spec.width == 640);
    CHECK(payload->spec.height == 480);
    const fs::path clip = dir.file("Ember Kettle Expedition (USA).mp4");
    REQUIRE(rv::test::writeClip(clip, *payload));

    const auto probed = rv::media::probeVideo(clip);
    REQUIRE(probed.has_value());
    CHECK(probed->codec == "mpeg4");
    CHECK(probed->codedSize.width == 640);
    CHECK(probed->codedSize.height == 480);
    CHECK(probed->durationSeconds == Catch::Approx(1.0).margin(0.15));

    rv::media::DecoderSettings settings;
    settings.loop = false;
    auto decoder = rv::test::startDecoder(clip, settings);
    const auto frames = rv::test::collectFrames(*decoder, 10);
    REQUIRE(frames.size() == 10);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        CHECK(frames[i].counter == static_cast<std::uint32_t>(i));
    }
    REQUIRE(rv::test::waitUntil([&] { return decoder->state() == DecoderState::Ended; }));
    CHECK(decoder->failure() == DecodeFailure::None);
    // Nothing is left beside it.
    CHECK(std::distance(fs::directory_iterator(dir.path()), fs::directory_iterator()) == 1);
}

TEST_CASE("support/fixture_payloads: damaged clips fail as asked", "[support][fixture][payloads]") {
    const rv::test::MediaTestDir dir;
    const auto write = [&](std::string_view name, std::string_view json) {
        const fs::path target = dir.file(name);
        auto payload = parseClip(json, rv::core::pathToUtf8(target.extension()));
        INFO(name);
        REQUIRE(payload.has_value());
        REQUIRE(rv::test::writeClip(target, *payload));
        return target;
    };
    const fs::path empty = write("empty.mp4", R"({"seed": 1, "damage": "empty"})");
    CHECK(fs::file_size(empty) == 0);
    CHECK(endOf(empty) == DecodeFailure::InvalidFile);

    const fs::path unsupported =
        write("unsupported.avi", R"({"seed": 2, "seconds": 1, "damage": "unsupported-codec"})");
    CHECK(endOf(unsupported) == DecodeFailure::UnsupportedCodec);

    const fs::path truncated = write("truncated.mp4", R"({"seed": 3, "damage": "truncated"})");
    CHECK(endOf(truncated) == DecodeFailure::Truncated);

    const fs::path corrupt = write("corrupt.mp4", R"({"seed": 4, "damage": "corrupt"})");
    CHECK(endOf(corrupt) == DecodeFailure::CorruptData);

    const fs::path mjpeg = write("motion.mkv", R"({"seed": 5, "seconds": 0.5, "codec": "mjpeg"})");
    CHECK(rv::media::probeVideo(mjpeg)->codec == "mjpeg");
    CHECK(endOf(mjpeg) == DecodeFailure::None);

    // Only the clips: the clean sources of the damaged ones are gone.
    CHECK(std::distance(fs::directory_iterator(dir.path()), fs::directory_iterator()) == 5);
}

TEST_CASE("support/fixture_payloads: files.json's video object", "[support][fixture][payloads]") {
    const auto full = parseClip(
        R"({"seed": 9, "seconds": 2.5, "width": 320, "height": 240, "frameRate": 25, "quantizer": 2,
            "damage": "corrupt"})");
    REQUIRE(full.has_value());
    CHECK(full->spec.seed == 9u);
    CHECK(full->spec.durationSeconds == 2.5);
    CHECK(full->spec.width == 320);
    CHECK(full->spec.frameRate == 25);
    CHECK(full->spec.keyFrameInterval == 25); // a key frame every second
    CHECK(full->spec.quantizer == 2);
    CHECK(full->damage == ClipDamage::Corrupt);
    CHECK(full->damageSeed == 9u);

    for (const std::string_view bad : {
             R"({})",                                         // no seed
             R"({"seed": 1, "width": 321})",                  // odd
             R"({"seed": 1, "seconds": 0})",                  // nothing to show
             R"({"seed": 1, "seconds": 61})",                 // too long
             R"({"seed": 1, "codec": "h264"})",               // no such encoder here
             R"({"seed": 1, "codec": "mjpeg"})",              // Motion JPEG never goes into an .mp4
             R"({"seed": 1, "damage": "unsupported-codec"})", // an .avi only
             R"({"seed": 1, "damage": "scratched"})",         // no such damage
             R"({"seed": 1, "audio": true})",                 // an unknown key
         }) {
        INFO(bad);
        const auto parsed = parseClip(bad);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == rv::ErrorCode::ValidationFailed);
    }
    CHECK_FALSE(parseClip(R"({"seed": 1})", ".webm").has_value()); // a container test_video cannot write
    CHECK(parseClip(R"({"seed": 1, "damage": "unsupported-codec"})", ".avi").has_value());
}

TEST_CASE("support/fixture_collection: a generated collection follows its rules", "[support][fixture][collection]") {
    rv::test::CollectionSpec spec;
    spec.seed = 2000;
    spec.titles = 2000;
    spec.duplicates = 300;
    spec.videos = 4;
    spec.romFolder = "roms/snes";
    spec.romExtension = ".sfc";
    spec.romSize = 4096;
    spec.gamelist = "ES-DE/gamelists/snes/gamelist.xml";
    spec.mediaFolder = "media/snes";
    const rv::test::GeneratedCollection collection = rv::test::generateCollection(spec);
    REQUIRE(collection.games.size() == 2300);

    std::set<std::string> titleKeys;
    std::set<std::string> stems;
    std::size_t representatives = 0;
    std::size_t europe = 0;
    std::size_t revisions = 0;
    std::size_t favorites = 0;
    std::size_t played = 0;
    std::size_t jpeg = 0;
    std::size_t portrait = 0;
    std::size_t videos = 0;
    for (const rv::test::CollectionGame& game : collection.games) {
        stems.insert(rv::core::toLowerAscii(game.stem));
        CHECK(game.stem.find(':') == std::string::npos);
        CHECK(game.cover.title == game.title);
        if (!game.representative) {
            continue;
        }
        ++representatives;
        titleKeys.insert(rv::core::toLowerAscii(game.title));
        favorites += game.favorite ? 1 : 0;
        played += game.playCount > 0 ? 1 : 0;
        jpeg += game.cover.format == PictureFormat::Jpeg ? 1 : 0;
        portrait += game.cover.height > game.cover.width ? 1 : 0;
        videos += game.hasVideo ? 1 : 0;
    }
    for (const rv::test::CollectionGame& game : collection.games) {
        if (!game.representative) {
            (game.tags == "(Europe)" ? europe : revisions) += 1;
            CHECK((game.tags == "(Europe)" || game.tags == "(USA)"));
        }
    }
    CHECK(stems.size() == 2300);     // every file name once, whatever the case
    CHECK(titleKeys.size() == 2000); // every title once
    CHECK(representatives == 2000);
    CHECK(europe == 200);
    CHECK(revisions == 100);
    CHECK(favorites == 80);
    CHECK(jpeg == 200);
    CHECK(portrait == 40);
    CHECK(videos == 4);
    CHECK(played > 400);
    CHECK(played < 600);
    CHECK(collection.gamelist.starts_with("<?xml version=\"1.0\"?>\n<gameList>\n"));

    // Deterministic, and another seed gives other titles.
    const rv::test::GeneratedCollection again = rv::test::generateCollection(spec);
    CHECK(again.gamelist == collection.gamelist);
    spec.seed = 2001;
    CHECK(rv::test::generateCollection(spec).gamelist != collection.gamelist);
}

TEST_CASE("support/fixture_collection: files.json's collection object", "[support][fixture][collection]") {
    const auto parse = [](std::string_view text) {
        return rv::test::parseCollectionSpec(nlohmann::json::parse(text), "a collection");
    };
    constexpr std::string_view kValid =
        R"({"seed": 1, "titles": 40, "duplicates": 6, "videos": 1, "romFolder": "roms/snes", "romExtension": ".sfc",
            "romSize": 16, "gamelist": "ES-DE/gamelists/snes/gamelist.xml", "mediaFolder": "media/snes"})";
    const auto valid = parse(kValid);
    REQUIRE(valid.has_value());
    CHECK(valid->titles == 40u);
    CHECK(valid->duplicates == 6u);

    nlohmann::json base = nlohmann::json::parse(kValid);
    const auto broken = [&](std::string_view key, nlohmann::json value) {
        nlohmann::json copy = base;
        if (value.is_null()) {
            copy.erase(std::string(key));
        } else {
            copy[std::string(key)] = std::move(value);
        }
        const auto parsed = rv::test::parseCollectionSpec(copy, "a collection");
        INFO(copy.dump());
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == rv::ErrorCode::ValidationFailed);
    };
    broken("seed", nullptr);
    broken("duplicates", 7); // 3 per 20 titles: at most 6 for 40
    broken("videos", 41);    // more than the titles
    broken("titles", 20001); // too many
    broken("romSize", 64ull * 1024ull * 1024ull + 1u);
    broken("romExtension", "sfc"); // no dot
    broken("gamelist", "");
    broken("covers", "media/snes/covers"); // an unknown key
}

TEST_CASE("support/fixture: generated media, collections and an overlay", "[support][fixture][payloads]") {
    const rv::test::TempDir dir;
    const fs::path source = dir.file("source");
    REQUIRE(rv::core::writeTextFileAtomic(source / "kept.txt", "kept").has_value());
    REQUIRE(rv::core::writeTextFileAtomic(source / "files.json", R"({
        "files": [
            {"path": "media/covers/A (USA).png", "cover": {"title": "A", "seed": 1, "width": 40, "height": 30},
             "mtime": "2001-02-03T04:05:06Z"},
            {"path": "media/covers/B (USA).jpg", "cover": {"title": "B", "seed": 2, "width": 30, "height": 40}},
            {"path": "media/videos/A (USA).mp4", "video": {"seed": 3, "seconds": 0.5, "width": 160, "height": 120}},
            {"path": "later/roms/C (USA).sfc", "size": 10, "seed": 4},
            {"path": "later/media/covers/A (USA).png", "cover": {"title": "A", "seed": 9, "width": 40, "height": 30}}
        ],
        "collections": [
            {"seed": 5, "titles": 20, "duplicates": 3, "videos": 1, "romFolder": "roms/x", "romExtension": ".x",
             "romSize": 8, "gamelist": "gamelists/x/gamelist.xml", "mediaFolder": "media/x"}
        ]
    })")
                .has_value());
    auto materialized = rv::test::Fixture::materializeFrom(source, "payloads");
    INFO((materialized ? "materialized" : materialized.error().toString()));
    REQUIRE(materialized.has_value());
    const rv::test::Fixture& fixture = *materialized;

    const auto a = rv::assets::loadImage(fixture.path("media/covers/A (USA).png"));
    REQUIRE(a.has_value());
    CHECK(a->width == 40);
    const auto b = rv::assets::loadImage(fixture.path("media/covers/B (USA).jpg"));
    REQUIRE(b.has_value());
    CHECK(b->height == 40);
    CHECK(rv::media::probeVideo(fixture.path("media/videos/A (USA).mp4"))->codedSize.width == 160);
    const auto written = rv::core::parseUtcIso8601("2001-02-03T04:05:06Z");
    REQUIRE(written.has_value());
    CHECK(fs::last_write_time(fixture.path("media/covers/A (USA).png")) ==
          std::chrono::clock_cast<std::chrono::file_clock>(*written));

    // The collection: 23 ROM files with covers, one clip, the gamelist.
    std::size_t roms = 0;
    for (const auto& entry : fs::directory_iterator(fixture.path("roms/x"))) {
        roms += entry.path().extension() == ".x" ? 1 : 0;
    }
    CHECK(roms == 23);
    CHECK(std::distance(fs::directory_iterator(fixture.path("media/x/covers")), fs::directory_iterator()) == 23);
    CHECK(std::distance(fs::directory_iterator(fixture.path("media/x/videos")), fs::directory_iterator()) == 1);
    CHECK(rv::core::readTextFile(fixture.path("gamelists/x/gamelist.xml"))->starts_with("<?xml"));

    // The overlay: a new ROM, a cover replaced, the folder gone.
    const auto before = rv::core::readBinaryFile(fixture.path("media/covers/A (USA).png"));
    REQUIRE(before.has_value());
    REQUIRE(fixture.applyOverlay("later"));
    CHECK(fs::exists(fixture.path("roms/C (USA).sfc")));
    CHECK(*rv::core::readBinaryFile(fixture.path("media/covers/A (USA).png")) != *before);
    CHECK_FALSE(fs::exists(fixture.path("later")));
    CHECK(fixture.applyOverlay("later").error().code == rv::ErrorCode::NotFound);
    CHECK(fixture.applyOverlay("../elsewhere").error().code == rv::ErrorCode::OutOfRange);
}

TEST_CASE("support/fixture: a file is one kind of payload", "[support][fixture][payloads]") {
    const rv::test::TempDir dir;
    const auto refused = [&](std::string_view manifest) {
        const fs::path source = dir.file("broken");
        fs::remove_all(source);
        REQUIRE(rv::core::writeTextFileAtomic(source / "files.json", manifest).has_value());
        const auto materialized = rv::test::Fixture::materializeFrom(source, "broken");
        INFO(manifest);
        REQUIRE_FALSE(materialized.has_value());
        return materialized.error();
    };
    CHECK(refused(R"({"files": [{"path": "a.png", "size": 1, "seed": 1, "cover": {"title": "A", "seed": 1}}]})").code ==
          rv::ErrorCode::ValidationFailed);
    CHECK(refused(R"({"files": [{"path": "a.mp4", "cover": {"title": "A", "seed": 1}, "video": {"seed": 1}}]})").code ==
          rv::ErrorCode::ValidationFailed);
    CHECK(refused(R"({"files": [{"path": "a.png", "cover": {"seed": 1}}]})").message.find("a.png") !=
          std::string::npos);
    CHECK(refused(R"({"files": [{"path": "a.mp4", "video": {"seed": 1, "codec": "h264"}}]})").code ==
          rv::ErrorCode::ValidationFailed);
    // A collection's file may not collide with a listed one.
    CHECK(refused(R"({"files": [{"path": "g/gamelist.xml", "size": 1, "seed": 1}],
                      "collections": [{"seed": 1, "titles": 1, "duplicates": 0, "videos": 0, "romFolder": "r",
                                       "romExtension": ".x", "romSize": 1, "gamelist": "g/gamelist.xml",
                                       "mediaFolder": "m"}]})")
              .message.find("listed twice") != std::string::npos);
}
