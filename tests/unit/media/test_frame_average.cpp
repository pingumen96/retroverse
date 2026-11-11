// A frame's mean colour (rv/media/video_frame.hpp, I12 step 6): its pixels on a 16 x 12 grid decoded from sRGB and
// averaged, as the hero screen's spill light reads it.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

#include <rv/media/video_frame.hpp>

using Catch::Approx;

TEST_CASE("media/frame average: the mean linear colour of a frame's grid", "[media][atmosphere]") {
    rv::media::VideoFrame frame;
    CHECK(rv::media::averageLinearColour(frame) == std::array<float, 3>{0.0f, 0.0f, 0.0f});

    REQUIRE(frame.resize(rv::media::FrameSize{64, 48}));
    // The left half pure red at full sRGB, the right half mid grey (sRGB 128, about 0.2158 linear).
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            std::uint8_t* pixel = frame.pixels() + static_cast<std::size_t>(y) * frame.strideBytes() +
                                  static_cast<std::size_t>(x) * rv::media::kFrameBytesPerPixel;
            const bool left = x < frame.width() / 2;
            pixel[0] = left ? 255 : 128;
            pixel[1] = left ? 0 : 128;
            pixel[2] = left ? 0 : 128;
            pixel[3] = 255;
        }
    }
    const std::array<float, 3> mean = rv::media::averageLinearColour(frame);
    CHECK(mean[0] == Approx(0.5f * 1.0f + 0.5f * 0.21586f).margin(1e-4));
    CHECK(mean[1] == Approx(0.5f * 0.21586f).margin(1e-4));
    CHECK(mean[2] == Approx(0.5f * 0.21586f).margin(1e-4));
}
