#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <cstdint>
#include <span>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/atmosphere.hpp>
#include <rv/scene/camera.hpp>

namespace rv::gl {

/// One emitter of a view's atmosphere (I12 step 6): its shape and its light as the scene holds it now (a light the
/// developer overlay or a sign's tint edited lights its dust alike).
struct AtmosphereSource {
    scene::AtmosphereEmitter emitter;
    glm::vec3 lightPosition{0.0f};
    glm::vec3 lightDirection{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensity = 0.0f;
    float range = 1.0f;
    float innerConeDeg = 0.0f;
    float outerConeDeg = 0.0f;
    bool spot = false;
};

/// How a view draws its atmosphere.
struct AtmosphereSettings {
    bool enabled = true;
    /// world.reduceMotion: the motes stand still (their time 0).
    bool frozen = false;
    /// Scales every emitter's motes (VR's fast profile: 0.5).
    float densityScale = 1.0f;
    /// The light cones (VR's fast profile: none).
    bool cones = true;
};

/// A mote's width, meters; the narrowest it is drawn, pixels (narrower, it is drawn this wide and dimmer, so that its
/// light stays the same and it never flickers between pixels).
inline constexpr float kMoteSizeM = 0.0025f;
inline constexpr float kMinMotePixels = 2.0f;
/// The share of its light's radiance at its place a mote sends towards the eye when it scatters evenly (dust/dust.vert
/// scatters forwards: about 2.5 times this looking into the light, a sixth of it with the light behind the eye).
inline constexpr float kMoteBrightness = 0.025f;

/// What a view's atmosphere pass draws.
struct AtmosphereFrame {
    AtmosphereUniforms uniforms;
    /// Instances of the dust draw (every emitter's motes) and emitters drawn as light cones.
    std::uint32_t motes = 0;
    std::uint32_t cones = 0;
};

/// The view's AtmosphereUniforms from `sources`: those whose bounds `frustum` sees, at most kMaxAtmosphereEmitters in
/// order, the light cones only with `settings.cones`, the motes scaled by densityScale (at least one for an emitter
/// with any), drifting at `time` seconds (0 when frozen); nothing when not enabled. `viewportSize` in pixels.
[[nodiscard]] AtmosphereFrame makeAtmosphereFrame(std::span<const AtmosphereSource> sources,
                                                  const AtmosphereSettings& settings, const scene::Frustum& frustum,
                                                  glm::ivec2 viewportSize, float time) noexcept;

} // namespace rv::gl
