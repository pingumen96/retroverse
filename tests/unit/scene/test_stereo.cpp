#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <glm/geometric.hpp>

#include <rv/core/math.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/stereo.hpp>

using Catch::Approx;
using rv::core::degreesToRadians;
using rv::scene::Camera;
using rv::scene::EyeFov;

TEST_CASE("scene/stereo: the eyes sit either side of the head and look where it looks (I10 step 1)",
          "[scene][stereo]") {
    Camera head;
    head.setPose(glm::vec3(1.0f, 1.6f, 2.0f), degreesToRadians(30.0f), degreesToRadians(-10.0f));
    const EyeFov field = rv::scene::stereoPreviewLeftField();
    const rv::scene::StereoEyes eyes = rv::scene::stereoEyesFor(head, 0.064f, field);

    CHECK(glm::distance(eyes.left.position(), eyes.right.position()) == Approx(0.064f));
    CHECK(glm::length(0.5f * (eyes.left.position() + eyes.right.position()) - head.position()) ==
          Approx(0.0f).margin(1e-6));
    // The left eye is to the head's left.
    CHECK(glm::dot(eyes.left.position() - head.position(), head.right()) < 0.0f);
    for (const Camera* eye : {&eyes.left, &eyes.right}) {
        REQUIRE(eye->eyeFov());
        CHECK(glm::dot(eye->forward(), head.forward()) == Approx(1.0f).margin(1e-5));
        CHECK(glm::dot(eye->up(), head.up()) == Approx(1.0f).margin(1e-5));
        CHECK(eye->lens().farPlane == head.lens().farPlane);
    }
    // The left eye sees farther to the left, the right eye to the right.
    CHECK(eyes.left.eyeFov()->left == Approx(field.left));
    CHECK(eyes.right.eyeFov()->right == Approx(-field.left));
    CHECK(eyes.right.eyeFov()->left == Approx(-field.right));
    CHECK(eyes.right.eyeFov()->up == Approx(field.up));
}

TEST_CASE("scene/stereo: side by side, each eye takes half the width at its field's proportions (I10 step 1)",
          "[scene][stereo]") {
    const EyeFov field = rv::scene::stereoPreviewLeftField();
    // The preview's field is about as wide as high (2.28 by 2.30 at distance 1).
    const rv::scene::StereoLayout wide = rv::scene::stereoLayout(glm::ivec2(1920, 1080), field);
    CHECK(wide.eyeSize.x == 960);
    CHECK(wide.eyeSize.y == Approx(960.0 * 2.30 / 2.28).margin(4.0));
    CHECK(wide.leftOffset == glm::ivec2(0, (1080 - wide.eyeSize.y) / 2));
    CHECK(wide.rightOffset == glm::ivec2(960, wide.leftOffset.y));

    // A short output: the height fits, each eye centered in its half.
    const rv::scene::StereoLayout shortOutput = rv::scene::stereoLayout(glm::ivec2(1920, 400), field);
    CHECK(shortOutput.eyeSize.y == 400);
    CHECK(shortOutput.eyeSize.x < 960);
    CHECK(shortOutput.leftOffset.x == (960 - shortOutput.eyeSize.x) / 2);
    CHECK(shortOutput.rightOffset.x == 960 + shortOutput.leftOffset.x);
    CHECK(shortOutput.leftOffset.y == 0);

    CHECK(rv::scene::stereoLayout(glm::ivec2(1, 100), field).eyeSize == glm::ivec2(0));
    CHECK(rv::scene::stereoLayout(glm::ivec2(100, 0), field).eyeSize == glm::ivec2(0));
}
