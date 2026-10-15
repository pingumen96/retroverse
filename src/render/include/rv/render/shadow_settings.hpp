#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <cstdint>
#include <optional>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/core/aabb.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/light.hpp>

namespace rv::gl {

/// Side of the key light's shadow map, in texels (GL_DEPTH_COMPONENT32F, 16 MiB).
inline constexpr int kShadowMapResolution = 2048;
/// Widest field of view of a spot light's shadow map, degrees. A wider cone is shadowed within this field only and
/// shines unshadowed beyond it: a perspective map that wide would spend its texels on its edges.
inline constexpr float kMaxSpotShadowFovDeg = 150.0f;
/// Narrowest field of view of a spot light's shadow map, degrees (a hard-edged pencil beam still gets a usable map).
inline constexpr float kMinSpotShadowFovDeg = 2.0f;
/// Near plane of a spot light's shadow map, meters (a tenth of the range for lights reaching less than half a meter):
/// casters closer to the light are clipped.
inline constexpr float kSpotShadowNearPlane = 0.05f;
/// Texels added around a directional light's fitted shadow map on every side, so that the filter footprint of the
/// casters at its edge stays inside it.
inline constexpr float kDirectionalShadowMarginTexels = 4.0f;
/// Largest depth bias factor and units of the shadow pass (ShadowSettings::slopeBias, constantBias).
inline constexpr float kMaxShadowDepthBias = 16.0f;
/// Largest normal offset, in texels of the map (ShadowSettings::normalOffsetTexels).
inline constexpr float kMaxShadowNormalOffsetTexels = 8.0f;

/// The light that casts the shadow of the views: the scene's key light, the first light with castsShadows, when it
/// reaches the lights block and can be shadow-mapped (a spot or the directional light; packLights() chooses it). It
/// carries what its shadow map needs; casts() is false when no light casts shadows.
struct ShadowLight {
    /// ShadowUniforms::lightIndex: the light's entry in LightsUniforms (a punctual light index, or
    /// kDirectionalShadowLight), kNoShadowLight for none.
    std::int32_t index = kNoShadowLight;
    scene::LightType type = scene::LightType::Spot;
    /// World position of a spot light, meters.
    glm::vec3 position{0.0f};
    /// Unit vector the light travels along.
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    /// Half-angle of a spot light's cone, degrees.
    float outerConeDeg = 0.0f;
    /// Reach of a spot light, meters: the far plane of its shadow map.
    float range = 0.0f;
    /// [0, 1]: share of the light's radiance its shadows take away (scene::Light::shadowStrength).
    float strength = 1.0f;

    [[nodiscard]] bool casts() const noexcept { return index != kNoShadowLight; }
};

/// How the renderer draws and reads the key light's shadow map: renderer tuning rather than scene content, edited live
/// by the developer overlay. The defaults suit room-scale scenes (texels of 3 to 8 mm on the furniture of a 9 x 10 m
/// room under a ceiling spot): the biases stay below a centimeter, so surfaces lit at grazing angles show no acne and
/// shadows stay attached to the objects that cast them (no peter-panning).
struct ShadowSettings {
    /// False: no shadow pass; the key light shines unshadowed.
    bool enabled = true;
    /// Depth bias of the shadow pass proportional to the depth slope of each caster triangle in the map
    /// (glPolygonOffset factor): the bias grows where surfaces face the light at grazing angles.
    float slopeBias = 2.0f;
    /// Constant depth bias of the shadow pass, in the smallest depth steps of the map (glPolygonOffset units).
    float constantBias = 2.0f;
    /// Receivers are looked up in the map from their position moved along their normal by this many texels of the map
    /// at their distance (normal offset): the comparison never lands on the receiver's own depth.
    float normalOffsetTexels = 1.5f;
};

/// The transforms of a shadow map.
struct ShadowProjection {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    /// World -> clip space of the map: projection * view.
    glm::mat4 viewProjection{1.0f};
    /// Eye of the map: the light's position for a perspective map, the light-space origin for an orthographic one.
    glm::vec3 eye{0.0f};
    bool perspective = false;
    /// World size of a texel: meters (orthographic), or meters per meter of distance from the eye (perspective).
    float texelWorldSize = 0.0f;
};

/// Perspective shadow map of a spot light (`light.type` Spot): from its position along its direction, with a square
/// field of view of twice its outer cone (clamped to [kMinSpotShadowFovDeg, kMaxSpotShadowFovDeg]), the near plane
/// kSpotShadowNearPlane (a tenth of the range for lights reaching less than half a meter) and the far plane at its
/// range. The view's up vector is world +Y, or -Z for lights pointing (nearly) straight up or down. Nothing for another
/// light type, a non-finite position or direction, a zero direction, a non-positive range or resolution.
[[nodiscard]] std::optional<ShadowProjection> spotShadowProjection(const ShadowLight& light, int resolution);

/// Orthographic shadow map of a directional light travelling along `direction`, fitted to `casterBounds` (the world
/// bounds of the scene's shadow casters: only they can shadow a point, so receivers outside the fit are lit): the
/// light space is a rotation of the world (no translation), the map is square, covers the bounds plus
/// kDirectionalShadowMarginTexels on every side, and its edges are snapped to whole texels of that light space, so that
/// bounds that move only slide the texel grid by whole texels (no shimmering). The depth range covers the bounds.
/// Nothing for a non-finite or zero direction, empty or non-finite bounds, or a non-positive resolution.
[[nodiscard]] std::optional<ShadowProjection> directionalShadowProjection(const glm::vec3& direction,
                                                                          const core::Aabb& casterBounds,
                                                                          int resolution);

/// The shadow map of `light`: spotShadowProjection() for a spot light, directionalShadowProjection() of its direction
/// and `casterBounds` for the directional light; nothing when it casts no shadow (ShadowLight::casts()), for a point
/// light, or when the projection is unusable.
[[nodiscard]] std::optional<ShadowProjection>
shadowProjectionFor(const ShadowLight& light, const core::Aabb& casterBounds, int resolution);

/// The ShadowUniforms of a view whose shadow map of `resolution` texels was drawn with `projection`: the map's
/// transform and eye, the light's index and strength (clamped to [0, 1], non-finite values counting as 1), one texel
/// (1 / resolution) and the normal offset of `settings` (normalOffsetTexels clamped to [0,
/// kMaxShadowNormalOffsetTexels], non-finite values counting as 0) in meters: constant for an orthographic map, growing
/// with the distance from the eye for a perspective one.
[[nodiscard]] ShadowUniforms makeShadowUniforms(const ShadowProjection& projection, const ShadowLight& light,
                                                const ShadowSettings& settings, int resolution) noexcept;

/// The depth bias of the shadow pass (glPolygonOffset factor, units): ShadowSettings::slopeBias and constantBias
/// clamped to [0, kMaxShadowDepthBias], non-finite values counting as 0.
[[nodiscard]] glm::vec2 shadowDepthBias(const ShadowSettings& settings) noexcept;

} // namespace rv::gl
