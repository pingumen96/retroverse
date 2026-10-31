// The CRT presets (I5 step 6, ADR-0016 §7): assets/materials/crt_presets.json, its built-in copy, the user's file over
// it key by key (never written), a file that does not load left out with an issue, and the screens' materials.

#include <rv/assets/crt_presets.hpp>
#include <rv/assets/material_library.hpp>

#include "support/temp_dir.hpp"

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using rv::ErrorCode;
using rv::assets::CrtEffect;
using rv::assets::CrtPreset;
using rv::assets::CrtPresets;

namespace {

const std::filesystem::path& assetRoot() {
    static const std::filesystem::path root = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return root;
}

nlohmann::json shipped() {
    auto document = rv::core::readJsonFile(assetRoot() / "materials" / "crt_presets.json");
    REQUIRE(document.has_value());
    return *document;
}

void writeUser(const rv::test::TempDir& dir, std::string_view text) {
    REQUIRE(rv::core::writeTextFileAtomic(dir.path() / "crt_presets.json", std::string(text)).has_value());
}

} // namespace

TEST_CASE("assets/crt_presets: the names", "[assets][crt_presets]") {
    for (const CrtPreset preset : rv::assets::kCrtPresets) {
        CHECK(rv::assets::crtPresetFromName(rv::assets::toString(preset)) == preset);
    }
    CHECK(rv::assets::toString(CrtPreset::Neutral) == "neutral");
    CHECK(rv::assets::toString(CrtPreset::Consumer) == "consumer");
    CHECK(rv::assets::toString(CrtPreset::Arcade) == "arcade");
    CHECK(rv::assets::toString(CrtPreset::Monitor) == "monitor");
    CHECK_FALSE(rv::assets::crtPresetFromName("").has_value());
    CHECK_FALSE(rv::assets::crtPresetFromName("Consumer").has_value());
    CHECK_FALSE(rv::assets::crtPresetFromName("vintage").has_value());
}

TEST_CASE("assets/crt_presets: the shipped file is the built-in copy, consumer today's tube", "[assets][crt_presets]") {
    auto parsed = rv::assets::parseCrtPresets(shipped(), "crt_presets.json");
    REQUIRE(parsed.has_value());
    const CrtPresets& builtIn = rv::assets::builtInCrtPresets();
    CHECK(*parsed == builtIn);

    // consumer: exactly the values every CRT screen had before the presets (MAT_Screen_CRT's of I3), CrtEffect's
    // defaults.
    const CrtEffect& consumer = builtIn[CrtPreset::Consumer];
    CHECK(consumer == CrtEffect{});
    CHECK(consumer == CrtEffect{.lineCount = 240.0f,
                                .scanlineStrength = 0.35f,
                                .maskTriadCount = 320.0f,
                                .maskStrength = 0.25f,
                                .curvature = 0.06f,
                                .vignette = 0.3f,
                                .glassReflection = 0.25f});

    // neutral: the clean video, no scanlines, mask, curvature or vignette (the glass stays).
    const CrtEffect& neutral = builtIn[CrtPreset::Neutral];
    CHECK(neutral.scanlineStrength == 0.0f);
    CHECK(neutral.maskStrength == 0.0f);
    CHECK(neutral.curvature == 0.0f);
    CHECK(neutral.vignette == 0.0f);
    CHECK(neutral.glassReflection == consumer.glassReflection);

    // arcade: deeper scanlines, a stronger mask, more curvature than consumer.
    const CrtEffect& arcade = builtIn[CrtPreset::Arcade];
    CHECK(arcade.scanlineStrength > consumer.scanlineStrength);
    CHECK(arcade.maskStrength > consumer.maskStrength);
    CHECK(arcade.curvature > consumer.curvature);
    CHECK(arcade.lineCount == consumer.lineCount);

    // monitor: a fine mask, little curvature.
    const CrtEffect& monitor = builtIn[CrtPreset::Monitor];
    CHECK(monitor.maskTriadCount > consumer.maskTriadCount);
    CHECK(monitor.maskStrength < consumer.maskStrength);
    CHECK(monitor.scanlineStrength < consumer.scanlineStrength);
    CHECK(monitor.curvature > 0.0f);
    CHECK(monitor.curvature < consumer.curvature);

    // Legible (style §15.2: the effect never hurts legibility): no gap darker than arcade's, no curvature beyond 0.1.
    for (const CrtPreset preset : rv::assets::kCrtPresets) {
        INFO(rv::assets::toString(preset));
        CHECK(builtIn[preset].scanlineStrength <= 0.5f);
        CHECK(builtIn[preset].maskStrength <= 0.35f);
        CHECK(builtIn[preset].curvature <= 0.1f);
        CHECK(builtIn[preset].glassReflection <= 0.35f);
    }
}

TEST_CASE("assets/crt_presets: the screens' materials mark a tube and keep the consumer values",
          "[assets][crt_presets][materials]") {
    auto library = rv::assets::MaterialLibrary::loadFromFile(assetRoot() / "materials/base/materials.json");
    REQUIRE(library.has_value());
    for (const std::string_view name : {"MAT_Screen_CRT", "MAT_Screen_CRT_Room", "MAT_Screen_Arcade"}) {
        INFO(name);
        const rv::assets::MaterialDesc* material = library->find(name);
        REQUIRE(material != nullptr);
        REQUIRE(material->crt.has_value());
        // Its look is its screen's preset (app::ScreenLooks); the material alone draws the consumer tube.
        CHECK(*material->crt == rv::assets::builtInCrtPresets()[CrtPreset::Consumer]);
    }
    // Every other material is no tube.
    for (const std::string& name : library->names()) {
        if (name != "MAT_Screen_CRT" && name != "MAT_Screen_CRT_Room" && name != "MAT_Screen_Arcade") {
            INFO(name);
            CHECK_FALSE(library->find(name)->crt.has_value());
        }
    }
}

TEST_CASE("assets/crt_presets: a CrtEffect object over its base", "[assets][crt_presets]") {
    const auto parse = [](std::string_view text) { return nlohmann::json::parse(text); };
    auto partial = rv::assets::parseCrtEffect(parse(R"({"curvature": 0.1})"), CrtEffect{}, false, "x");
    REQUIRE(partial.has_value());
    CrtEffect expected;
    expected.curvature = 0.1f;
    CHECK(*partial == expected);

    auto missing = rv::assets::parseCrtEffect(parse(R"({"curvature": 0.1})"), CrtEffect{}, true, "x");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::ParseError);
    CHECK(missing.error().context == "x/lineCount");

    struct Case {
        std::string_view text;
        ErrorCode code;
        std::string_view where;
    };
    const std::vector<Case> cases = {
        {R"({"lineCount": 8})", ErrorCode::OutOfRange, "x/lineCount"},
        {R"({"scanlineStrength": 1.5})", ErrorCode::OutOfRange, "x/scanlineStrength"},
        {R"({"maskTriadCount": 5000})", ErrorCode::OutOfRange, "x/maskTriadCount"},
        {R"({"maskStrength": -0.1})", ErrorCode::OutOfRange, "x/maskStrength"},
        {R"({"curvature": 0.6})", ErrorCode::OutOfRange, "x/curvature"},
        {R"({"vignette": "deep"})", ErrorCode::ParseError, "x/vignette"},
        {R"({"glow": 1})", ErrorCode::ParseError, "x/glow"},
        {R"([1])", ErrorCode::ParseError, "x"},
    };
    for (const Case& c : cases) {
        INFO(c.text);
        auto parsed = rv::assets::parseCrtEffect(parse(c.text), CrtEffect{}, false, "x");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == c.code);
        CHECK(parsed.error().context == c.where);
    }
}

TEST_CASE("assets/crt_presets: the user's file over the shipped one, key by key, never written",
          "[assets][crt_presets]") {
    rv::test::TempDir dir("crt-presets-user");
    // No user file: the shipped values.
    {
        const rv::assets::LoadedCrtPresets loaded = rv::assets::loadCrtPresets(assetRoot(), dir.path());
        CHECK(loaded.issues.empty());
        CHECK(loaded.source == "shipped");
        CHECK(loaded.presets == rv::assets::builtInCrtPresets());
    }
    writeUser(dir, R"({
  // comments are allowed
  "schemaVersion": 1,
  "presets": {"arcade": {"scanlineStrength": 0.42}, "neutral": {"glassReflection": 0}}
})");
    const std::string before = *rv::core::readTextFile(dir.path() / "crt_presets.json");
    const rv::assets::LoadedCrtPresets loaded = rv::assets::loadCrtPresets(assetRoot(), dir.path());
    CHECK(loaded.issues.empty());
    CHECK(loaded.source == "user");
    CrtPresets expected = rv::assets::builtInCrtPresets();
    expected[CrtPreset::Arcade].scanlineStrength = 0.42f;
    expected[CrtPreset::Neutral].glassReflection = 0.0f;
    CHECK(loaded.presets == expected);
    CHECK(*rv::core::readTextFile(dir.path() / "crt_presets.json") == before); // never written
}

TEST_CASE("assets/crt_presets: a user file that does not load is left out whole", "[assets][crt_presets]") {
    rv::test::TempDir dir("crt-presets-bad");
    struct Case {
        std::string_view text;
        ErrorCode code;
        std::string_view where;
    };
    const std::vector<Case> cases = {
        {R"({"presets": {"arcade": {"scanlineStrength": 2}}})", ErrorCode::OutOfRange,
         "/presets/arcade/scanlineStrength"},
        {R"({"presets": {"vintage": {}}})", ErrorCode::ParseError, "/presets/vintage"},
        {R"({"presets": {"monitor": {"glow": 1}}})", ErrorCode::ParseError, "/presets/monitor/glow"},
        {R"({"presets": {"consumer": 1}})", ErrorCode::ParseError, "/presets/consumer"},
        {R"({"schemaVersion": 2})", ErrorCode::Unsupported, "/schemaVersion"},
        {R"({"extra": true})", ErrorCode::ParseError, "/extra"},
        {R"([1, 2])", ErrorCode::ParseError, ""},
        {R"({"presets": )", ErrorCode::ParseError, ""},
    };
    for (const Case& c : cases) {
        INFO(c.text);
        writeUser(dir, c.text);
        const rv::assets::LoadedCrtPresets loaded = rv::assets::loadCrtPresets(assetRoot(), dir.path());
        CHECK(loaded.source == "shipped");
        CHECK(loaded.presets == rv::assets::builtInCrtPresets());
        REQUIRE(loaded.issues.size() == 1);
        CHECK(loaded.issues.front().code == c.code);
        CHECK(loaded.issues.front().context.find(c.where) != std::string::npos);
    }
}

TEST_CASE("assets/crt_presets: a shipped file that does not load leaves the built-in values", "[assets][crt_presets]") {
    rv::test::TempDir root("crt-presets-root");
    rv::test::TempDir user("crt-presets-root-user");
    // Missing: the built-in values, an issue.
    {
        const rv::assets::LoadedCrtPresets loaded = rv::assets::loadCrtPresets(root.path(), user.path());
        CHECK(loaded.source == "built-in");
        CHECK(loaded.presets == rv::assets::builtInCrtPresets());
        CHECK(loaded.issues.size() == 1);
    }
    // Incomplete (a preset without a key): the same, and the user's file is not read over it.
    std::filesystem::create_directories(root.path() / "materials");
    nlohmann::json document = shipped();
    document["presets"]["monitor"].erase("curvature");
    REQUIRE(rv::core::writeTextFileAtomic(root.path() / "materials" / "crt_presets.json", document.dump()).has_value());
    writeUser(user, R"({"presets": {"monitor": {"curvature": 0.03}}})");
    const rv::assets::LoadedCrtPresets loaded = rv::assets::loadCrtPresets(root.path(), user.path());
    CHECK(loaded.source == "built-in");
    CHECK(loaded.presets == rv::assets::builtInCrtPresets());
    REQUIRE(loaded.issues.size() == 1);
    CHECK(loaded.issues.front().code == ErrorCode::ParseError);
    CHECK(loaded.issues.front().context.find("/presets/monitor/curvature") != std::string::npos);
}
