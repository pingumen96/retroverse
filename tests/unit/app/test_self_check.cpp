#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <span>
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
    CHECK(rv::app::toString(SelfCheck::Fxaa) == "fxaa");
    CHECK(rv::app::selfCheckFromString("srgb") == SelfCheck::Srgb);
    CHECK(rv::app::selfCheckFromString("bloom") == SelfCheck::Bloom);
    CHECK(rv::app::selfCheckFromString("fxaa") == SelfCheck::Fxaa);
    for (const SelfCheck check : rv::app::kSelfChecks) {
        CHECK(rv::app::selfCheckFromString(rv::app::toString(check)) == check);
    }
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

namespace {

constexpr int kEdgeSize = 16;

/// A kEdgeSize-square RGBA image: black left of a stair-stepped edge (column 6 + row / 4), white right of it.
std::vector<std::uint8_t> hardEdge() {
    std::vector<std::uint8_t> pixels;
    for (int y = 0; y < kEdgeSize; ++y) {
        for (int x = 0; x < kEdgeSize; ++x) {
            const std::uint8_t value = x < 6 + y / 4 ? 0 : 255;
            pixels.insert(pixels.end(), {value, value, value, 255});
        }
    }
    return pixels;
}

/// Sets pixel (x, y) of a kEdgeSize-square RGBA image to grey `value`.
void setGrey(std::vector<std::uint8_t>& pixels, int x, int y, std::uint8_t value) {
    const auto offset = static_cast<std::size_t>((y * kEdgeSize + x) * 4);
    pixels[offset] = value;
    pixels[offset + 1] = value;
    pixels[offset + 2] = value;
}

/// `hardEdge()` with the last black pixel of the first `rows` rows blended to grey, as FXAA leaves a slanted edge.
std::vector<std::uint8_t> blendedEdge(int rows) {
    std::vector<std::uint8_t> pixels = hardEdge();
    for (int y = 0; y < rows; ++y) {
        setGrey(pixels, 5 + y / 4, y, 96);
    }
    return pixels;
}

} // namespace

TEST_CASE("app/self_check: the FXAA check passes a blended edge with untouched flat areas", "[app][self_check]") {
    const auto result = rv::app::evaluateFxaaCheck(hardEdge(), blendedEdge(kEdgeSize), kEdgeSize, kEdgeSize);
    CHECK(result.passed);
    CHECK(result.aliasedIsHard);
    CHECK(result.edgeRows == 16);
    CHECK(result.blendedEdgeRows == 16);
    CHECK(result.edgeBlended);
    CHECK(result.flatCopied);
    CHECK(result.flatChanged == 0);
    // Every pixel at least two columns from the edge has a flat 3 x 3 neighborhood.
    CHECK(result.flatPixels >= static_cast<std::size_t>(kEdgeSize * (kEdgeSize - 4)));
    CHECK(result.flatPixels < static_cast<std::size_t>(kEdgeSize * kEdgeSize));

    // Half the rows blended is enough: the middle row of a stair-step may keep its hard values.
    CHECK(rv::app::evaluateFxaaCheck(hardEdge(), blendedEdge(8), kEdgeSize, kEdgeSize).passed);
    const auto fewer = rv::app::evaluateFxaaCheck(hardEdge(), blendedEdge(7), kEdgeSize, kEdgeSize);
    CHECK(fewer.blendedEdgeRows == 7);
    CHECK_FALSE(fewer.edgeBlended);
    CHECK_FALSE(fewer.passed);
}

TEST_CASE("app/self_check: the FXAA check fails what made the anti-aliasing wrong", "[app][self_check]") {
    // A pass that copies everything (the edge stays aliased).
    const auto copied = rv::app::evaluateFxaaCheck(hardEdge(), hardEdge(), kEdgeSize, kEdgeSize);
    CHECK(copied.flatCopied);
    CHECK(copied.blendedEdgeRows == 0);
    CHECK_FALSE(copied.passed);

    // A pass that blurs flat areas too (or a dithered input): one changed pixel far from the edge is enough.
    std::vector<std::uint8_t> blurred = blendedEdge(kEdgeSize);
    setGrey(blurred, 0, 0, 1);
    const auto changed = rv::app::evaluateFxaaCheck(hardEdge(), blurred, kEdgeSize, kEdgeSize);
    CHECK(changed.flatChanged == 1);
    CHECK_FALSE(changed.flatCopied);
    CHECK_FALSE(changed.passed);

    // An input that is not a hard edge proves nothing (dithering or bloom left on).
    std::vector<std::uint8_t> soft = hardEdge();
    setGrey(soft, 15, 15, 254);
    const auto notHard = rv::app::evaluateFxaaCheck(soft, blendedEdge(kEdgeSize), kEdgeSize, kEdgeSize);
    CHECK_FALSE(notHard.aliasedIsHard);
    CHECK_FALSE(notHard.passed);

    // A uniform input has no edge.
    const std::vector<std::uint8_t> uniform = greyPixels(kEdgeSize * kEdgeSize, 40);
    const auto flat = rv::app::evaluateFxaaCheck(uniform, uniform, kEdgeSize, kEdgeSize);
    CHECK_FALSE(flat.aliasedIsHard);
    CHECK(flat.edgeRows == 0);
    CHECK_FALSE(flat.edgeBlended);
    CHECK_FALSE(flat.passed);

    // Read-backs that do not hold the target, or an empty target.
    const std::vector<std::uint8_t> edge = hardEdge();
    CHECK_FALSE(rv::app::evaluateFxaaCheck(edge, std::span(edge).first(edge.size() - 4), kEdgeSize, kEdgeSize).passed);
    CHECK_FALSE(rv::app::evaluateFxaaCheck(edge, edge, 0, kEdgeSize).passed);
}

TEST_CASE("app/self_check: the FXAA check needs the in-place write equal to the direct one", "[app][self_check]") {
    const auto passing = [] {
        return rv::app::evaluateFxaaCheck(hardEdge(), blendedEdge(kEdgeSize), kEdgeSize, kEdgeSize);
    };
    std::vector<std::uint8_t> bloomed = blendedEdge(kEdgeSize);
    setGrey(bloomed, 2, 3, 17);

    auto equal = passing();
    REQUIRE(equal.passed);
    rv::app::evaluateFxaaInPlaceWrite(equal, bloomed, bloomed, kEdgeSize, kEdgeSize);
    CHECK(equal.inPlacePixels == static_cast<std::size_t>(kEdgeSize * kEdgeSize));
    CHECK(equal.inPlaceChanged == 0);
    CHECK(equal.inPlaceExact);
    CHECK(equal.passed);

    // One byte apart, the alpha (the luma the anti-aliasing reads) included.
    std::vector<std::uint8_t> alpha = bloomed;
    alpha[(5 * kEdgeSize + 1) * 4 + 3] = 254;
    auto differs = passing();
    rv::app::evaluateFxaaInPlaceWrite(differs, bloomed, alpha, kEdgeSize, kEdgeSize);
    CHECK(differs.inPlaceChanged == 1);
    CHECK_FALSE(differs.inPlaceExact);
    CHECK_FALSE(differs.passed);

    // A failed FXAA check stays failed; read-backs that do not hold the target fail.
    auto failed = rv::app::evaluateFxaaCheck(hardEdge(), hardEdge(), kEdgeSize, kEdgeSize);
    rv::app::evaluateFxaaInPlaceWrite(failed, bloomed, bloomed, kEdgeSize, kEdgeSize);
    CHECK(failed.inPlaceExact);
    CHECK_FALSE(failed.passed);
    auto truncated = passing();
    rv::app::evaluateFxaaInPlaceWrite(truncated, bloomed, std::span(bloomed).first(bloomed.size() - 4), kEdgeSize,
                                      kEdgeSize);
    CHECK(truncated.inPlacePixels == 0);
    CHECK_FALSE(truncated.inPlaceExact);
    CHECK_FALSE(truncated.passed);
}
