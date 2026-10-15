#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.

#include <span>
#include <string>
#include <vector>

#include <glm/vec2.hpp>

#include <rv/render/shadow_settings.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/light.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::gl {

/// A scene light that packLights() left out of the lights block.
struct DroppedLight {
    /// Light::name, or "#<index in the scene's light list>" when the light has none.
    std::string name;
    std::string reason;
};

/// LightsUniforms of a scene, the lights that are not in it, and the light that casts its shadows.
struct PackedLights {
    LightsUniforms uniforms;
    std::vector<DroppedLight> dropped;
    /// The key light: the first scene light with castsShadows, when it is in the block and is a spot or the
    /// directional light; casts() is false otherwise.
    ShadowLight shadowLight;
    /// Why the first light with castsShadows casts no shadow (a point light, a light left out of the block); empty
    /// when it casts one or no light asks to.
    std::string shadowNote;
};

/// Builds the lights block of a scene (contract of rv::scene::Light, layout of LightsUniforms):
///  - point and spot lights in list order, up to kMaxPunctualLights; later ones are dropped;
///  - the first directional light; later ones are dropped (a scene without one gets directionalIntensity 0);
///  - the environment's ambient sky, ground and intensity (its exposure belongs to the post-processing:
///    postSettingsFor() in post_settings.hpp);
///  - the key light, the first light with castsShadows (PackedLights::shadowLight): its index in the block, position,
///    direction, outer cone, range and shadow strength (clamped to [0, 1], non-finite values counting as 1). A point
///    key light casts no shadow (point-light shadows are not supported: key lights are spots), and neither does a
///    dropped one; shadowNote then says why. Later lights with castsShadows cast none.
/// Lights with a non-finite or negative value (position, direction, color, intensity, range, cone) are dropped
/// instead of reaching the shaders; ranges below 1 mm count as 1 mm and directions are normalized. The scene loader
/// already rejects such lights, so this only guards hand-made input. Unused entries stay zero (intensity 0).
[[nodiscard]] PackedLights packLights(std::span<const scene::Light> lights, const scene::EnvironmentSetup& environment);

/// PunctualLightUniforms::spotScale and spotOffset (x, y) of a spot cone with half-angles `innerConeDeg` <=
/// `outerConeDeg` (degrees): the cone factor is 1 inside the inner cone and 0 outside the outer one. An inner angle
/// larger than the outer one is treated as equal (a hard edge).
[[nodiscard]] glm::vec2 spotConeScaleOffset(float innerConeDeg, float outerConeDeg) noexcept;

} // namespace rv::gl
