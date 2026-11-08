// I11 step 7: a console model of the user's content (assets::parseConsoleManifest, checkConsoleModel,
// compressConsoleTextures, consoleMediaSocket, loadConsoleContent; BC3).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <rv/assets/bc1.hpp>
#include <rv/assets/console_content.hpp>
#include <rv/assets/gltf_loader.hpp>
#include <rv/core/file_io.hpp>

#include "support/fixture_console.hpp"
#include "support/temp_dir.hpp"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using rv::assets::ConsoleManifest;

namespace {

rv::assets::ModelData fixtureModel(const rv::test::TempDir& dir, const rv::test::FixtureConsoleOptions& options = {}) {
    const auto file = dir.path() / "console.glb";
    REQUIRE(rv::test::writeFixtureConsole(file, options).has_value());
    auto model = rv::assets::loadGltf(file);
    REQUIRE(model.has_value());
    return std::move(*model);
}

rv::assets::ImageData solid(int side, std::uint8_t alpha) {
    rv::assets::ImageData image;
    image.width = side;
    image.height = side;
    image.channels = 4;
    image.pixels.resize(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4, 200);
    for (std::size_t at = 3; at < image.pixels.size(); at += 4) {
        image.pixels[at] = alpha;
    }
    return image;
}

} // namespace

TEST_CASE("assets/console_content: console.json, read and written", "[assets][console_content]") {
    SECTION("every key, back as written") {
        ConsoleManifest manifest;
        manifest.kind = rv::assets::ConsoleKind::Handheld;
        manifest.front = rv::assets::ConsoleFront::MinusX;
        manifest.sockets = {{"CartridgeSlot", "Slot_Node"}};
        manifest.scaleCm = glm::vec3(24.0F, 7.0F, 20.0F);
        manifest.source = {"https://example.invalid/m", "Someone", "CC-BY-4.0", "2026-11-08", "decimated"};
        const std::string text = rv::assets::writeConsoleManifest(manifest);
        auto read = rv::assets::parseConsoleManifest(text, "console.json");
        REQUIRE(read.has_value());
        CHECK(*read == manifest);
    }
    SECTION("the defaults: the model console.glb, home, front +z") {
        auto read = rv::assets::parseConsoleManifest("// a comment\n{\"schemaVersion\": 1}", "console.json");
        REQUIRE(read.has_value());
        CHECK(read->model == "console.glb");
        CHECK(read->kind == rv::assets::ConsoleKind::Home);
        CHECK(read->front == rv::assets::ConsoleFront::PlusZ);
    }
    SECTION("refused") {
        for (const std::string_view text :
             {"{}", "{\"schemaVersion\": 2}", "{\"schemaVersion\": 1, \"extra\": 1}",
              "{\"schemaVersion\": 1, \"model\": \"../x.glb\"}", "{\"schemaVersion\": 1, \"model\": \"C:x.glb\"}",
              "{\"schemaVersion\": 1, \"kind\": \"arcade\"}", "{\"schemaVersion\": 1, \"front\": \"up\"}",
              "{\"schemaVersion\": 1, \"sockets\": {\"Lid\": \"n\"}}", "{\"schemaVersion\": 1, \"scaleCm\": [1, 2]}",
              "{\"schemaVersion\": 1, \"source\": {\"licence\": \"x\"}}", "[1]", "not json"}) {
            CAPTURE(text);
            CHECK_FALSE(rv::assets::parseConsoleManifest(text, "console.json").has_value());
        }
    }
}

TEST_CASE("assets/console_content: the checks", "[assets][console_content]") {
    const rv::test::TempDir dir;
    const ConsoleManifest manifest;
    SECTION("the fixture console passes: 24 x 7 x 20 cm, its textures compressed within 2 MiB") {
        const auto model = fixtureModel(dir);
        const auto check = rv::assets::checkConsoleModel(model, manifest);
        CHECK(check.ok());
        CHECK(check.size.x == Approx(0.24F).margin(0.005F));
        CHECK(check.size.y == Approx(0.07F).margin(0.005F));
        CHECK(check.size.z == Approx(0.20F).margin(0.005F));
        CHECK(check.triangles > 0);
        // A 256 x 256 opaque base colour: BC1 with its mips.
        CHECK(check.textureBytes == rv::assets::compressedTextureBytes(model));
        CHECK(check.textureBytes > 256u * 256u / 2u);
        CHECK(check.textureBytes < 256u * 256u);
    }
    SECTION("too large, a footprint past the cabinet, scaleCm not met") {
        rv::test::FixtureConsoleOptions twice;
        twice.scale = 2.0F;
        const auto check = rv::assets::checkConsoleModel(fixtureModel(dir, twice), manifest);
        REQUIRE_FALSE(check.ok());
        CHECK_THAT(check.problems.front(), ContainsSubstring("48.0 cm wide"));
        ConsoleManifest scaled;
        scaled.scaleCm = glm::vec3(30.0F, 7.0F, 20.0F);
        const auto off = rv::assets::checkConsoleModel(fixtureModel(dir), scaled);
        REQUIRE(off.problems.size() == 1);
        CHECK_THAT(off.problems.front(), ContainsSubstring("scaleCm"));
    }
    SECTION("turned by its front: +x makes its depth its width") {
        ConsoleManifest turned;
        turned.front = rv::assets::ConsoleFront::PlusX;
        const auto check = rv::assets::checkConsoleModel(fixtureModel(dir), turned);
        CHECK(check.size.x == Approx(0.20F).margin(0.005F));
        CHECK(check.size.z == Approx(0.24F).margin(0.005F));
    }
    SECTION("textures over 2 MiB once compressed") {
        rv::test::FixtureConsoleOptions large;
        large.imageSide = 2048;
        const auto check = rv::assets::checkConsoleModel(fixtureModel(dir, large), manifest);
        REQUIRE_FALSE(check.ok());
        CHECK_THAT(check.problems.back(), ContainsSubstring("2 MiB at most"));
    }
    SECTION("a socket named in the manifest that the model lacks") {
        ConsoleManifest named;
        named.sockets = {{"DiscTray", "Tray"}};
        const auto check = rv::assets::checkConsoleModel(fixtureModel(dir), named);
        REQUIRE(check.problems.size() == 1);
        CHECK_THAT(check.problems.front(), ContainsSubstring("\"Tray\""));
    }
}

TEST_CASE("assets/console_content: the textures compressed, BC1 and BC3", "[assets][console_content]") {
    CHECK(rv::assets::bc3LevelBytes(5, 4) == 32);
    CHECK(rv::assets::encodeBc3(solid(8, 128)).size() == 64);
    CHECK(rv::assets::isOpaque(solid(4, 255)));
    CHECK_FALSE(rv::assets::isOpaque(solid(4, 254)));

    const rv::test::TempDir dir;
    rv::assets::ModelData model = fixtureModel(dir);
    auto textures = rv::assets::compressConsoleTextures(model);
    REQUIRE(textures.size() == 1);
    CHECK(textures.front().srgb);
    CHECK(textures.front().format == rv::assets::TextureBlocks::Bc1Srgb);
    // 256 down to 1: nine levels, each whole blocks.
    REQUIRE(textures.front().levels.size() == 9);
    CHECK(textures.front().levels.back().width == 1);
    CHECK(textures.front().bytes() == rv::assets::compressedTextureBytes(model));
    // A base colour with transparency is BC3.
    model.images.front() = solid(64, 100);
    textures = rv::assets::compressConsoleTextures(model);
    REQUIRE(textures.size() == 1);
    CHECK(textures.front().format == rv::assets::TextureBlocks::Bc3Srgb);
    CHECK(textures.front().levels.front().blocks.size() == rv::assets::bc3LevelBytes(64, 64));
}

TEST_CASE("assets/console_content: the media socket and the placement", "[assets][console_content]") {
    const rv::test::TempDir dir;
    const ConsoleManifest manifest;
    SECTION("the model's Anchor_CartridgeSlot, on the console's top") {
        const auto model = fixtureModel(dir);
        const auto socket = rv::assets::consoleMediaSocket(model, manifest);
        CHECK(socket.from == "CartridgeSlot");
        const glm::vec3 at(socket.transform[3]);
        CHECK(at.y == Approx(0.07F).margin(0.002F));
        // The placement puts the bounds' bottom middle at the origin.
        const glm::vec3 origin(rv::assets::consolePlacement(model, manifest.front) *
                               glm::vec4(0.5F * (model.bounds.min.x + model.bounds.max.x), model.bounds.min.y,
                                         0.5F * (model.bounds.min.z + model.bounds.max.z), 1.0F));
        CHECK(glm::length(origin) < 1e-5F);
    }
    SECTION("without an anchor: the top of its bounds' middle, no opening") {
        rv::test::FixtureConsoleOptions bare;
        bare.slotAnchor = false;
        const auto socket = rv::assets::consoleMediaSocket(fixtureModel(dir, bare), manifest);
        CHECK(socket.from == "top");
        CHECK(socket.opening == glm::vec2(0.0F));
        CHECK(glm::vec3(socket.transform[3]).y == Approx(0.07F).margin(0.002F));
    }
}

TEST_CASE("assets/console_content: a console's folder", "[assets][console_content]") {
    const rv::test::TempDir dir;
    const auto folder = rv::assets::consoleFolder(dir.path(), "snes");
    CHECK(folder == dir.path() / "consoles" / "snes");
    CHECK(rv::assets::loadConsoleContent(folder).error().code == rv::ErrorCode::NotFound);
    REQUIRE(rv::test::writeFixtureConsole(folder / "console.glb").has_value());
    REQUIRE(rv::core::writeTextFileAtomic(folder / "console.json", rv::assets::writeConsoleManifest({})).has_value());
    auto content = rv::assets::loadConsoleContent(folder);
    REQUIRE(content.has_value());
    CHECK(content->manifest.model == "console.glb");
    CHECK_FALSE(content->model.meshes.empty());
}
