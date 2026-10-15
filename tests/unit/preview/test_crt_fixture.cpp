#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "support/temp_dir.hpp"
#include <rv/assets/glb_writer.hpp>
#include <rv/assets/gltf_loader.hpp>
#include <rv/core/math.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/preview/crt_fixture.hpp>

using Catch::Approx;
using rv::geometry::SocketKind;

TEST_CASE("preview/fixture: the CRT-like fixture loads with its sockets, on the floor, facing +Z",
          "[preview][fixture]") {
    rv::test::TempDir dir;
    const auto file = dir.file("crt_fixture.glb");
    REQUIRE(rv::assets::writeGlb(file, rv::preview::makeCrtFixture()).has_value());
    const auto model = rv::assets::loadGltf(file);
    REQUIRE(model.has_value());
    CHECK(model->warnings.empty());

    // 0.71 x 0.61 x 0.56 m, pivot on the floor at the middle of the footprint, front +Z.
    CHECK(rv::core::nearlyEqual(model->bounds.size(), rv::preview::kCrtFixtureSize, 1e-5f));
    CHECK(model->bounds.min.y == Approx(0.0f).margin(1e-6));
    CHECK(model->bounds.center().x == Approx(0.0f).margin(1e-6));
    CHECK(model->bounds.center().z == Approx(0.0f).margin(1e-6));

    const auto socket = [&](std::string_view id) {
        return rv::geometry::findSocket(model->sockets, id);
    };
    REQUIRE(model->sockets.size() == 4);
    const auto* screen = socket("ScreenSurface");
    REQUIRE(screen != nullptr);
    CHECK(screen->kind == SocketKind::ScreenSurface);
    REQUIRE(screen->meshIndex.has_value());
    CHECK(screen->bounds.size().x / screen->bounds.size().y == Approx(4.0f / 3.0f));
    CHECK(glm::vec3(screen->transform[3]).z == Approx(model->bounds.max.z)); // the screen is the front
    const auto* stand = socket("TV_Stand");
    REQUIRE(stand != nullptr);
    CHECK(stand->kind == SocketKind::Anchor);
    CHECK(rv::core::nearlyEqual(glm::vec3(stand->transform[3]), glm::vec3(0.0f)));
    const auto* look = socket("ScreenLook");
    REQUIRE(look != nullptr);
    CHECK(glm::vec3(look->transform[3]).z > model->bounds.max.z); // in front of the screen
    const auto* collider = socket("ColliderProxy");
    REQUIRE(collider != nullptr);
    CHECK(collider->kind == SocketKind::ColliderProxy);

    // Body and screen are drawn, the collision proxy is not; the body has a texture for the inspector.
    CHECK(model->drawItems.size() == 2);
    REQUIRE(model->images.size() == 1);
    CHECK(model->images[0].width == 32);
    CHECK(model->materials.size() == 2);
    CHECK(model->materials[0].baseColorTexture.imageIndex == std::optional<std::uint32_t>(0));
}

TEST_CASE("preview/fixture: the fixture is the same on every run", "[preview][fixture]") {
    const auto first = rv::assets::buildGlb(rv::preview::makeCrtFixture());
    const auto second = rv::assets::buildGlb(rv::preview::makeCrtFixture());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
}
