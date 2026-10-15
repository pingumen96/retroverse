#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
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
    CHECK(rv::app::toString(SelfCheck::Bloom) == "bloom");
    CHECK(rv::app::selfCheckFromString("srgb") == SelfCheck::Srgb);
    CHECK(rv::app::selfCheckFromString("bloom") == SelfCheck::Bloom);
    CHECK_FALSE(rv::app::selfCheckFromString("SRGB").has_value());
    CHECK_FALSE(rv::app::selfCheckFromString("Bloom").has_value());
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

namespace {

/// Measurements of a bloom chain that works: nothing from the lit square, the emitter's energy nearly all kept, eight
/// fireflies within a few percent of each other.
rv::app::BloomCheckMeasurements goodBloom() {
    rv::app::BloomCheckMeasurements measurements;
    measurements.litBloomPeak = 0.0f;
    measurements.emitterEnergyRatio = 0.987f;
    measurements.fireflyEnergies = {4.6f, 4.6f, 4.6f, 4.6f, 4.8f, 4.8f, 4.8f, 4.8f};
    return measurements;
}

} // namespace

TEST_CASE("app/self_check: the bloom check passes a chain that blooms as documented", "[app][self_check]") {
    const auto result = rv::app::evaluateBloomCheck(goodBloom());
    CHECK(result.passed);
    CHECK(result.litStaysOut);
    CHECK(result.energyKept);
    CHECK(result.firefliesSteady);
    CHECK(result.fireflySpread > 0.02f);
    CHECK(result.fireflySpread < 0.03f);
}

TEST_CASE("app/self_check: the bloom check fails what made the bloom wrong", "[app][self_check]") {
    // Lit paper that blooms: any glow from the lit square.
    auto lit = goodBloom();
    lit.litBloomPeak = 1e-3f;
    CHECK_FALSE(rv::app::evaluateBloomCheck(lit).litStaysOut);
    CHECK_FALSE(rv::app::evaluateBloomCheck(lit).passed);

    // Light added or lost by the chain (unnormalized filters, a broken blend), within 5% only.
    for (const float ratio : {0.94f, 1.06f, 0.5f, 2.0f, 0.0f, std::numeric_limits<float>::quiet_NaN()}) {
        INFO(ratio);
        auto energy = goodBloom();
        energy.emitterEnergyRatio = ratio;
        CHECK_FALSE(rv::app::evaluateBloomCheck(energy).energyKept);
        CHECK_FALSE(rv::app::evaluateBloomCheck(energy).passed);
    }
    for (const float ratio : {0.96f, 1.0f, 1.04f}) {
        auto energy = goodBloom();
        energy.emitterEnergyRatio = ratio;
        CHECK(rv::app::evaluateBloomCheck(energy).passed);
    }

    // A firefly whose glow follows its brightness (Karis weights per box: 1000x glows ten times as much as 100x).
    auto pulsing = goodBloom();
    pulsing.fireflyEnergies = {6.0f, 6.0f, 6.0f, 6.0f, 60.0f, 60.0f, 60.0f, 60.0f};
    const auto failed = rv::app::evaluateBloomCheck(pulsing);
    CHECK_FALSE(failed.firefliesSteady);
    CHECK_FALSE(failed.passed);
    CHECK(failed.fireflySpread > 0.8f);
    // Every energy within 20% of their mean is steady; one beyond it is not.
    auto steady = goodBloom();
    steady.fireflyEnergies = {1.0f, 1.0f, 1.0f, 1.25f}; // the last one 17.6% above the mean
    CHECK(rv::app::evaluateBloomCheck(steady).firefliesSteady);
    auto unsteady = goodBloom();
    unsteady.fireflyEnergies = {1.0f, 1.0f, 1.0f, 1.35f}; // 24% above
    CHECK_FALSE(rv::app::evaluateBloomCheck(unsteady).firefliesSteady);
    // Fewer than two energies, no glow at all or a non-finite one prove nothing.
    const float infinity = std::numeric_limits<float>::infinity();
    for (const std::vector<float>& energies : {std::vector<float>{}, std::vector<float>{4.6f},
                                               std::vector<float>{0.0f, 0.0f}, std::vector<float>{4.6f, infinity}}) {
        INFO(energies.size() << " energies");
        auto unproven = goodBloom();
        unproven.fireflyEnergies = energies;
        CHECK_FALSE(rv::app::evaluateBloomCheck(unproven).firefliesSteady);
        CHECK_FALSE(rv::app::evaluateBloomCheck(unproven).passed);
    }
}
