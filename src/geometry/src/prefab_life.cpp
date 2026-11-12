// I12 step 8's life (ADR-0021 §6): a lava lamp, its wax breathing (the app's FlickerMode::Pulse on the wax's
// material). Of original design.

#include <string>

#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::translation;

constexpr std::string_view kLavaLamp = "lava_lamp";
constexpr int kLampSegments = 28;

/// The lamp's three stacked shapes, shares of its height: the base cone, the bottle, the cap.
constexpr float kBaseShare = 0.30f;
constexpr float kBottleShare = 0.55f;
constexpr float kCapShare = 0.15f;

Status validate(const LavaLampParams& p) {
    return firstError({checkRange(kLavaLamp, "height", p.height, 0.25f, 0.7f),
                       checkRange(kLavaLamp, "radius", p.radius, 0.04f, 0.12f)});
}

ProceduralPrefab assemble(const LavaLampParams& p) {
    detail::PrefabAssembler out(kLavaLamp, {"metal", "glass", "wax"});
    const float r = p.radius;
    const float base = kBaseShare * p.height;
    const float bottle = kBottleShare * p.height;
    const float cap = kCapShare * p.height;
    MeshBuilder& metal = out.part("metal");
    // The base widens to the floor; the cap narrows to a point's stub.
    metal.frustum(r, 0.6f * r, base, kLampSegments, true, false, translation(glm::vec3(0.0f, 0.5f * base, 0.0f)));
    metal.frustum(0.38f * r, 0.18f * r, cap, kLampSegments, true, false,
                  translation(glm::vec3(0.0f, base + bottle + 0.5f * cap, 0.0f)));
    // The bottle tapers upwards from the base's top.
    const float bottleBottom = 0.58f * r;
    const float bottleTop = 0.36f * r;
    out.part("glass").frustum(bottleBottom, bottleTop, bottle, kLampSegments, true, false,
                              translation(glm::vec3(0.0f, base + 0.5f * bottle, 0.0f)));
    // The wax: a pool on the bottle's floor and three blobs rising, each inside the bottle at its height.
    MeshBuilder& wax = out.part("wax");
    const float pool = 0.08f * bottle;
    wax.frustum(0.5f * r, 0.46f * r, pool, kLampSegments, true, false,
                translation(glm::vec3(0.0f, base + 0.004f + 0.5f * pool, 0.0f)));
    struct Blob {
        float x;
        float share; ///< of the bottle's height
        float radius;
    };
    for (const Blob blob : {Blob{0.08f, 0.32f, 0.24f}, Blob{-0.1f, 0.58f, 0.17f}, Blob{0.05f, 0.8f, 0.12f}}) {
        wax.uvSphere(blob.radius * r, 16, 10,
                     translation(glm::vec3(blob.x * r, base + blob.share * bottle, 0.0f)));
    }
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildLavaLamp(const LavaLampParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
