#include <catch2/catch_test_macros.hpp>

#include <limits>

#include <rv/media/output_size.hpp>

using rv::media::FrameSize;
using rv::media::scaledFrameSize;

TEST_CASE("media/output_size: frames taller than the limit shrink to it, keeping the aspect ratio",
          "[media][output_size]") {
    CHECK(scaledFrameSize({1920, 1080}, 1.0, 720) == FrameSize{1280, 720});
    CHECK(scaledFrameSize({3840, 2160}, 1.0, 720) == FrameSize{1280, 720});
    CHECK(scaledFrameSize({1920, 1080}, 1.0, 480) == FrameSize{853, 480});
    CHECK(scaledFrameSize({1440, 1080}, 1.0, 720) == FrameSize{960, 720});
}

TEST_CASE("media/output_size: frames are never enlarged", "[media][output_size]") {
    CHECK(scaledFrameSize({320, 240}, 1.0, 720) == FrameSize{320, 240});
    CHECK(scaledFrameSize({1280, 720}, 1.0, 720) == FrameSize{1280, 720});
    CHECK(scaledFrameSize({256, 224}, 1.0, 0) == FrameSize{256, 224});
    CHECK(scaledFrameSize({4096, 2160}, 1.0, -1) == FrameSize{4096, 2160});
}

TEST_CASE("media/output_size: non-square pixels shrink the side they stretch", "[media][output_size]") {
    // 720x480 with 8:9 pixels is a 4:3 picture: 640x480.
    CHECK(scaledFrameSize({720, 480}, 8.0 / 9.0, 720) == FrameSize{640, 480});
    // 720x480 with 32:27 pixels is a 16:9 picture: the height shrinks instead of the width growing.
    CHECK(scaledFrameSize({720, 480}, 32.0 / 27.0, 720) == FrameSize{720, 405});
    // Then the limit applies.
    CHECK(scaledFrameSize({1440, 1080}, 4.0 / 3.0, 720) == FrameSize{1280, 720});
}

TEST_CASE("media/output_size: unusable aspects mean square pixels, and sides stay at least one pixel",
          "[media][output_size]") {
    CHECK(scaledFrameSize({640, 480}, 0.0, 720) == FrameSize{640, 480});
    CHECK(scaledFrameSize({640, 480}, -2.0, 720) == FrameSize{640, 480});
    CHECK(scaledFrameSize({640, 480}, std::numeric_limits<double>::quiet_NaN(), 720) == FrameSize{640, 480});
    CHECK(scaledFrameSize({640, 480}, std::numeric_limits<double>::infinity(), 720) == FrameSize{640, 480});
    CHECK(scaledFrameSize({2, 1000}, 1.0, 10) == FrameSize{1, 10});
    CHECK_FALSE(scaledFrameSize({0, 480}, 1.0, 720).valid());
    CHECK_FALSE(scaledFrameSize({640, -1}, 1.0, 720).valid());
}
