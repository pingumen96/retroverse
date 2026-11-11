// The atmosphere pass's block (rv/render/atmosphere_settings.hpp, I12 step 6): the emitters the view's frustum sees, in
// order, their motes one after another (firstMote), scaled by the density, the light cones only when asked, the motes'
// time held at 0 while frozen, the light's radiance and cone terms, at most kMaxAtmosphereEmitters.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <glm/glm.hpp>

#include <rv/render/atmosphere_settings.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/scene/camera.hpp>

using rv::gl::AtmosphereFrame;
using rv::gl::AtmosphereSettings;
using rv::gl::AtmosphereSource;
using rv::scene::AtmosphereKind;

namespace {

/// An emitter of `kind` at `apex` (straight down, 2 m) whose light is a warm 10 spot.
AtmosphereSource source(AtmosphereKind kind, glm::vec3 apex, std::uint32_t motes) {
    AtmosphereSource result;
    result.emitter.kind = kind;
    result.emitter.apex = apex;
    result.emitter.axis = glm::vec3(0.0f, -1.0f, 0.0f);
    result.emitter.length = 2.0f;
    result.emitter.tanHalfAngle = 0.5f;
    result.emitter.motes = kind == AtmosphereKind::LightCone ? 0 : motes;
    result.emitter.strength = kind == AtmosphereKind::LightCone ? 0.035f : 0.0f;
    result.emitter.seed = 7;
    result.emitter.bounds = rv::core::Aabb::fromMinMax(apex - glm::vec3(1.0f, 2.0f, 1.0f), apex + glm::vec3(1.0f));
    result.lightPosition = apex;
    result.color = glm::vec3(1.0f, 0.8f, 0.6f);
    result.intensity = 10.0f;
    result.range = 6.0f;
    result.innerConeDeg = 20.0f;
    result.outerConeDeg = 30.0f;
    result.spot = true;
    return result;
}

/// The view from the origin down -Z.
rv::scene::Frustum frustum() {
    rv::scene::Camera camera;
    camera.setAspect(16.0f / 9.0f);
    camera.setPose(glm::vec3(0.0f, 1.6f, 0.0f), 0.0f, 0.0f);
    return camera.frustum();
}

} // namespace

TEST_CASE("render/atmosphere: the frame's emitters are those in view, their motes one after another",
          "[render][atmosphere]") {
    const std::vector<AtmosphereSource> sources{
        source(AtmosphereKind::DustCone, {0.0f, 3.0f, -3.0f}, 320),
        source(AtmosphereKind::LightCone, {0.0f, 3.0f, -3.0f}, 0),
        source(AtmosphereKind::DustPool, {1.0f, 2.5f, -4.0f}, 160),
        // Behind the eye: culled.
        source(AtmosphereKind::DustPool, {0.0f, 2.5f, 6.0f}, 160),
    };
    const AtmosphereFrame frame =
        rv::gl::makeAtmosphereFrame(sources, AtmosphereSettings{}, frustum(), {1920, 1080}, 12.5f);
    REQUIRE(frame.uniforms.emitterCount == 3);
    CHECK(frame.motes == 480);
    CHECK(frame.cones == 1);
    const auto& emitters = frame.uniforms.emitters;
    CHECK(emitters[0].kind == rv::gl::atmosphere_kind::kDustCone);
    CHECK(emitters[0].firstMote == 0);
    CHECK(emitters[0].motes == 320);
    CHECK(emitters[1].kind == rv::gl::atmosphere_kind::kLightCone);
    CHECK(emitters[1].motes == 0);
    CHECK(emitters[1].strength == 0.035f);
    CHECK(emitters[2].kind == rv::gl::atmosphere_kind::kDustPool);
    CHECK(emitters[2].firstMote == 320);
    CHECK(emitters[2].motes == 160);
    // The light as the view lights it.
    CHECK(emitters[0].radiance == glm::vec3(10.0f, 8.0f, 6.0f));
    const glm::vec2 cone = rv::gl::spotConeScaleOffset(20.0f, 30.0f);
    CHECK(emitters[0].spotScale == cone.x);
    CHECK(emitters[0].spotOffset == cone.y);
    CHECK(frame.uniforms.time == 12.5f);
    CHECK(frame.uniforms.viewportSize == glm::vec2(1920.0f, 1080.0f));
    CHECK(frame.uniforms.moteSize == rv::gl::kMoteSizeM);
    CHECK(frame.uniforms.minMotePixels == rv::gl::kMinMotePixels);

    // VR's fast profile: half the motes, no cones.
    AtmosphereSettings fast;
    fast.densityScale = 0.5f;
    fast.cones = false;
    const AtmosphereFrame halved = rv::gl::makeAtmosphereFrame(sources, fast, frustum(), {2048, 2048}, 12.5f);
    CHECK(halved.uniforms.emitterCount == 2);
    CHECK(halved.motes == 240);
    CHECK(halved.cones == 0);
    CHECK(halved.uniforms.emitters[1].firstMote == 160);

    // Frozen (reduceMotion): the motes stand at time 0.
    AtmosphereSettings frozen;
    frozen.frozen = true;
    CHECK(rv::gl::makeAtmosphereFrame(sources, frozen, frustum(), {1920, 1080}, 12.5f).uniforms.time == 0.0f);

    // Off, or a light turned off: nothing.
    AtmosphereSettings off;
    off.enabled = false;
    CHECK(rv::gl::makeAtmosphereFrame(sources, off, frustum(), {1920, 1080}, 1.0f).uniforms.emitterCount == 0);
    std::vector<AtmosphereSource> dark = sources;
    for (AtmosphereSource& entry : dark) {
        entry.intensity = 0.0f;
    }
    CHECK(rv::gl::makeAtmosphereFrame(dark, AtmosphereSettings{}, frustum(), {1920, 1080}, 1.0f).motes == 0);

    // At most kMaxAtmosphereEmitters.
    const std::vector<AtmosphereSource> many(40, source(AtmosphereKind::DustCone, {0.0f, 3.0f, -3.0f}, 10));
    const AtmosphereFrame full = rv::gl::makeAtmosphereFrame(many, AtmosphereSettings{}, frustum(), {800, 600}, 0.0f);
    CHECK(full.uniforms.emitterCount == rv::gl::kMaxAtmosphereEmitters);
    CHECK(full.motes == 10 * rv::gl::kMaxAtmosphereEmitters);
}
