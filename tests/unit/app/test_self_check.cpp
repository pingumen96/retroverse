#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <rv/app/self_check.hpp>

using rv::app::SelfCheck;

namespace {

/// `count` RGBA pixels of grey `value` with an arbitrary alpha.
std::vector<std::uint8_t> greyPixels(std::size_t count, std::uint8_t value) {
    std::vector<std::uint8_t> pixels;
    for (std::size_t i = 0; i < count; ++i) {
        pixels.insert(pixels.end(), {value, value, value, static_cast<std::uint8_t>(i)});
    }
    return pixels;
}

} // namespace

TEST_CASE("app/self_check: check names", "[app][self_check]") {
    CHECK(rv::app::toString(SelfCheck::Srgb) == "srgb");
    CHECK(rv::app::selfCheckFromString("srgb") == SelfCheck::Srgb);
    CHECK_FALSE(rv::app::selfCheckFromString("SRGB").has_value());
    CHECK_FALSE(rv::app::selfCheckFromString("").has_value());
}

TEST_CASE("app/self_check: an sRGB target stores linear 0.5 as 188", "[app][self_check]") {
    CHECK(rv::app::kSrgbCheckLinearValue == 0.5f);
    CHECK(rv::app::expectedSrgbByte(0.5f) == 188);
    CHECK(rv::app::expectedSrgbByte(0.0f) == 0);
    CHECK(rv::app::expectedSrgbByte(1.0f) == 255);
    CHECK(rv::app::expectedSrgbByte(0.2158605f) == 128); // "#808080"
}

TEST_CASE("app/self_check: the sRGB check tolerates driver rounding only", "[app][self_check]") {
    const auto passed = rv::app::evaluateSrgbCheck(greyPixels(64, 188));
    CHECK(passed.passed);
    CHECK(passed.expected == 188);
    CHECK(passed.minimum == 188);
    CHECK(passed.maximum == 188);
    CHECK(passed.pixelCount == 64);

    std::vector<std::uint8_t> pixels = greyPixels(4, 188);
    pixels[0] = 186; // red of the first pixel, within 2
    pixels[9] = 190; // green of the third pixel, within 2
    CHECK(rv::app::evaluateSrgbCheck(pixels).passed);

    for (const std::uint8_t wrong : {std::uint8_t{128}, std::uint8_t{185}, std::uint8_t{191}, std::uint8_t{255}}) {
        INFO(int{wrong});
        std::vector<std::uint8_t> bad = greyPixels(4, 188);
        bad[6] = wrong; // blue of the second pixel
        const auto result = rv::app::evaluateSrgbCheck(bad);
        CHECK_FALSE(result.passed);
        CHECK((result.minimum == wrong || result.maximum == wrong));
    }
    // Linear output stored without encoding (no sRGB conversion) reads 128 and fails.
    CHECK_FALSE(rv::app::evaluateSrgbCheck(greyPixels(64, 128)).passed);
    // No pixels, or only a partial one, prove nothing.
    CHECK_FALSE(rv::app::evaluateSrgbCheck({}).passed);
    const std::vector<std::uint8_t> partial{188, 188, 188};
    CHECK_FALSE(rv::app::evaluateSrgbCheck(partial).passed);
}
