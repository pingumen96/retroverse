#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <fstream>
#include <vector>

#include "support/temp_dir.hpp"
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

using Catch::Approx;

TEST_CASE("core/file_io: atomic write then read back", "[core][file_io]") {
    rv::test::TempDir dir;
    const auto path = dir.file("nested/folder/config.json");

    REQUIRE(rv::core::writeTextFileAtomic(path, "{\"a\": 1}").has_value());
    const auto text = rv::core::readTextFile(path);
    REQUIRE(text.has_value());
    CHECK(*text == "{\"a\": 1}");

    // Replacing an existing file works and leaves no temporary behind.
    REQUIRE(rv::core::writeTextFileAtomic(path, "second").has_value());
    CHECK(rv::core::readTextFile(path).value() == "second");
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        ++entries;
    }
    CHECK(entries == 1);

    const std::vector<std::byte> bytes{std::byte{0}, std::byte{255}, std::byte{13}, std::byte{10}};
    const auto binaryPath = dir.file("data.bin");
    REQUIRE(rv::core::writeFileAtomic(binaryPath, bytes).has_value());
    CHECK(rv::core::readBinaryFile(binaryPath).value() == bytes);
    CHECK(rv::core::lastWriteTime(binaryPath).has_value());
}

TEST_CASE("core/file_io: errors", "[core][file_io]") {
    rv::test::TempDir dir;
    CHECK(rv::core::readTextFile(dir.file("missing.txt")).error().code == rv::ErrorCode::NotFound);
    CHECK(rv::core::readTextFile(dir.path()).error().code == rv::ErrorCode::InvalidArgument);
    CHECK_FALSE(rv::core::lastWriteTime(dir.file("missing.txt")).has_value());

    const auto big = dir.file("big.txt");
    REQUIRE(rv::core::writeTextFileAtomic(big, std::string(1024, 'x')).has_value());
    CHECK(rv::core::readTextFile(big, 100).error().code == rv::ErrorCode::OutOfRange);

    const auto empty = dir.file("empty.txt");
    REQUIRE(rv::core::writeTextFileAtomic(empty, "").has_value());
    CHECK(rv::core::readTextFile(empty).value().empty());
}

TEST_CASE("core/json: parsing with comments and typed vectors", "[core][json]") {
    const auto doc = rv::core::parseJson(R"({
        // hand-written files may contain comments
        "position": [1.0, 2.5, -3],
        "uv": [0.25, 0.75],
        "color": [1, 0.5, 0.25, 1]
    })",
                                         "inline");
    REQUIRE(doc.has_value());
    const auto position = rv::core::jsonToVec3((*doc)["position"], "inline:/position");
    REQUIRE(position.has_value());
    CHECK(position->z == Approx(-3.0f));
    CHECK(rv::core::jsonToVec2((*doc)["uv"], "uv").value().y == Approx(0.75f));
    CHECK(rv::core::jsonToVec4((*doc)["color"], "color").value().y == Approx(0.5f));

    const auto wrongSize = rv::core::jsonToVec3((*doc)["uv"], "inline:/uv");
    REQUIRE_FALSE(wrongSize.has_value());
    CHECK(wrongSize.error().context == "inline:/uv");
    CHECK_FALSE(rv::core::jsonToVec2(nlohmann::json::array({1, "x"}), "mixed").has_value());

    const auto broken = rv::core::parseJson("{ \"a\": ", "broken.json");
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code == rv::ErrorCode::ParseError);
    CHECK(broken.error().context == "broken.json");

    CHECK(rv::core::toJson(glm::vec3(1.0f, 2.0f, 3.0f)) == nlohmann::json::array({1.0f, 2.0f, 3.0f}));
}

TEST_CASE("core/json: reading files", "[core][json]") {
    rv::test::TempDir dir;
    const auto path = dir.file("scene.json");
    REQUIRE(rv::core::writeTextFileAtomic(path, R"({"name": "m0_testroom"})").has_value());
    const auto doc = rv::core::readJsonFile(path);
    REQUIRE(doc.has_value());
    CHECK((*doc)["name"] == "m0_testroom");
    CHECK(rv::core::readJsonFile(dir.file("missing.json")).error().code == rv::ErrorCode::NotFound);
}

TEST_CASE("core/text: UTF-8 paths and string helpers", "[core][text]") {
    const std::string utf8 = "Pok\xC3\xA9mon \xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3/cover.png";
    const auto path = rv::core::pathFromUtf8(utf8);
    CHECK(rv::core::pathToUtf8(path) == utf8);
    CHECK(path.filename() == "cover.png");

    rv::test::TempDir dir;
    const auto unicodeFile = dir.path() / rv::core::pathFromUtf8("\xC3\xA9t\xC3\xA9.txt");
    REQUIRE(rv::core::writeTextFileAtomic(unicodeFile, "ok").has_value());
    CHECK(rv::core::readTextFile(unicodeFile).value() == "ok");

    CHECK(rv::core::toLowerAscii("SNES Room") == "snes room");
    CHECK(rv::core::equalsIgnoreCaseAscii("ScreenSurface", "screensurface"));
    CHECK_FALSE(rv::core::equalsIgnoreCaseAscii("abc", "abcd"));
    CHECK(rv::core::trim("  \t value \r\n") == "value");
    CHECK(rv::core::trim("   ").empty());
    const auto parts = rv::core::split("a,,b", ',');
    REQUIRE(parts.size() == 3);
    CHECK(parts[1].empty());
    CHECK(parts[2] == "b");
}
