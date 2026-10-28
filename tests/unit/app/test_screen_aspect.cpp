// The picture's aspect on its screen (I5 step 4, ADR-0016 §7): letterboxed or pillarboxed in the tube's black, never
// stretched; a 4:3 clip on a 4:3 screen keeps the whole screen (its pixels unchanged).

#include <rv/app/screen_aspect.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

using rv::app::isWholeScreen;
using rv::app::kWholeScreen;
using rv::app::pictureRect;

namespace {

/// The screens of the building: a CRT's 4:3 tube (crt prefab, screenAspect 4/3; the arcade's hero cabinet's 0.50 x
/// 0.375 m), an arcade cabinet's portrait screen (the family's 298 x 398 mm label).
constexpr glm::vec2 kCrt{0.48f, 0.36f};
constexpr glm::vec2 kHeroCabinet{0.50f, 0.375f};
constexpr glm::vec2 kCabinet{0.298f, 0.398f};

/// The clips as the decoder delivers them at 480 rows (square pixels, the stream's sample aspect applied).
constexpr glm::vec2 kClip43{640.0f, 480.0f};
constexpr glm::vec2 kClip169{854.0f, 480.0f};
constexpr glm::vec2 kClipPortrait{360.0f, 480.0f}; // 3:4, a vertical arcade monitor's

struct Row {
    std::string name;
    glm::vec2 clip;
    glm::vec2 screen;
    glm::vec4 rect;
};

} // namespace

TEST_CASE("app/screen aspect: the picture rectangle's table", "[app][screens][aspect]") {
    // 16:9 on 4:3: the whole width, 0.75 of the height ((4 / 3) / (16 / 9)), bars of 0.125 above and below. A portrait
    // clip on a 4:3 tube: the whole height, 0.5625 of the width. On a cabinet's 298 x 398 mm screen (0.749) a portrait
    // clip fills it (within the tolerance); a 4:3 clip there is letterboxed to 0.562 of its height, a 16:9 one to
    // 0.421.
    const std::vector<Row> rows{
        {"4:3 on a CRT", kClip43, kCrt, kWholeScreen},
        {"4:3 on the hero cabinet", kClip43, kHeroCabinet, kWholeScreen},
        {"16:9 on a CRT", kClip169, kCrt, {0.0f, 0.125f, 1.0f, 0.75f}},
        {"portrait on a CRT", kClipPortrait, kCrt, {0.21875f, 0.0f, 0.5625f, 1.0f}},
        {"portrait on a cabinet", kClipPortrait, kCabinet, kWholeScreen},
        {"4:3 on a cabinet", kClip43, kCabinet, {0.0f, 0.219221f, 1.0f, 0.561558f}},
        {"16:9 on a cabinet", kClip169, kCabinet, {0.0f, 0.289580f, 1.0f, 0.420840f}},
    };
    for (const Row& row : rows) {
        INFO(row.name);
        const glm::vec4 rect = pictureRect(row.clip, row.screen);
        CHECK(rect.x == Catch::Approx(row.rect.x).margin(2e-3));
        CHECK(rect.y == Catch::Approx(row.rect.y).margin(2e-3));
        CHECK(rect.z == Catch::Approx(row.rect.z).margin(2e-3));
        CHECK(rect.w == Catch::Approx(row.rect.w).margin(2e-3));
        // Centred, inside the screen, and of the clip's own aspect on the screen's shape (never stretched).
        CHECK(rect.x + 0.5f * rect.z == Catch::Approx(0.5f));
        CHECK(rect.y + 0.5f * rect.w == Catch::Approx(0.5f));
        CHECK(rect.z <= 1.0f);
        CHECK(rect.w <= 1.0f);
        if (!isWholeScreen(rect)) {
            const double shown = (rect.z * row.screen.x) / (rect.w * row.screen.y);
            CHECK(shown == Catch::Approx(row.clip.x / row.clip.y).epsilon(1e-4));
        }
    }
}

TEST_CASE("app/screen aspect: the whole screen is exact for the same aspect, within the tolerance and without sizes",
          "[app][screens][aspect]") {
    // A 4:3 clip on a 4:3 screen: exactly (0, 0, 1, 1), the material's flag left clear (its pixels unchanged).
    CHECK(isWholeScreen(pictureRect(kClip43, kCrt)));
    CHECK(isWholeScreen(pictureRect({320.0f, 240.0f}, {4.0f, 3.0f})));
    // An encoder's crop within 2 %: 640 x 478 and 584 x 446 (1.8 % narrower).
    CHECK(isWholeScreen(pictureRect({640.0f, 478.0f}, kCrt)));
    CHECK(isWholeScreen(pictureRect({584.0f, 446.0f}, kCrt)));
    // Past it the clip keeps its aspect: 256 x 224 (8:7, 14 % narrower) is pillarboxed.
    const glm::vec4 eightBySeven = pictureRect({256.0f, 224.0f}, kCrt);
    CHECK_FALSE(isWholeScreen(eightBySeven));
    CHECK(eightBySeven.w == 1.0f);
    CHECK(eightBySeven.z == Catch::Approx(0.857143f).margin(1e-4));
    // Nothing known: the screen as it is.
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
    CHECK(isWholeScreen(pictureRect({0.0f, 0.0f}, kCrt)));
    CHECK(isWholeScreen(pictureRect(kClip169, {0.0f, 0.36f})));
    CHECK(isWholeScreen(pictureRect({kNan, 480.0f}, kCrt)));
    CHECK(isWholeScreen(pictureRect(kClip169, {-1.0f, 1.0f})));
}
