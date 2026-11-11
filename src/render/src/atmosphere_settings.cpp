#include <rv/render/atmosphere_settings.hpp>

#include <algorithm>
#include <cmath>

#include <rv/render/light_packing.hpp>

namespace rv::gl {

AtmosphereFrame makeAtmosphereFrame(std::span<const AtmosphereSource> sources, const AtmosphereSettings& settings,
                                    const scene::Frustum& frustum, glm::ivec2 viewportSize, float time) noexcept {
    AtmosphereFrame frame;
    AtmosphereUniforms& block = frame.uniforms;
    block.viewportSize = glm::vec2(std::max(viewportSize.x, 1), std::max(viewportSize.y, 1));
    block.time = settings.frozen || !std::isfinite(time) ? 0.0f : time;
    block.moteSize = kMoteSizeM;
    block.moteBrightness = kMoteBrightness;
    block.minMotePixels = kMinMotePixels;
    if (!settings.enabled) {
        return frame;
    }
    const float density = std::isfinite(settings.densityScale) ? std::clamp(settings.densityScale, 0.0f, 4.0f) : 1.0f;
    for (const AtmosphereSource& source : sources) {
        if (block.emitterCount >= kMaxAtmosphereEmitters) {
            break;
        }
        const scene::AtmosphereEmitter& emitter = source.emitter;
        const bool cone = emitter.kind == scene::AtmosphereKind::LightCone;
        if ((cone && !settings.cones) || !(source.intensity > 0.0f) || !frustum.intersects(emitter.bounds)) {
            continue;
        }
        std::uint32_t motes = 0;
        if (!cone) {
            if (emitter.motes == 0 || density <= 0.0f) {
                continue;
            }
            motes = std::max<std::uint32_t>(
                1u, static_cast<std::uint32_t>(std::lround(static_cast<double>(emitter.motes) * density)));
        }
        AtmosphereEmitterUniforms& entry = block.emitters[block.emitterCount++];
        entry.apex = emitter.apex;
        entry.length = emitter.length;
        entry.axis = emitter.axis;
        entry.tanHalfAngle = emitter.tanHalfAngle;
        entry.lightPosition = source.lightPosition;
        entry.range = std::max(source.range, 0.01f);
        entry.lightDirection = source.lightDirection;
        entry.radius = emitter.radius;
        entry.radiance = source.color * source.intensity;
        entry.strength = emitter.strength;
        if (source.spot) {
            const glm::vec2 terms = spotConeScaleOffset(source.innerConeDeg, source.outerConeDeg);
            entry.spotScale = terms.x;
            entry.spotOffset = terms.y;
        } else {
            entry.spotScale = 0.0f;
            entry.spotOffset = 1.0f;
        }
        entry.kind = cone                                              ? atmosphere_kind::kLightCone
                     : emitter.kind == scene::AtmosphereKind::DustPool ? atmosphere_kind::kDustPool
                                                                       : atmosphere_kind::kDustCone;
        entry.firstMote = frame.motes;
        entry.motes = motes;
        entry.seed = emitter.seed;
        frame.motes += motes;
        frame.cones += cone ? 1u : 0u;
    }
    return frame;
}

} // namespace rv::gl
