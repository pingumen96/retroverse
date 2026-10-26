// I6 step 5's hub (ADR-0014 §3, style §5.2, §8): the colonnade's columns, the balcony's railing, the central landmark
// (a glowing globe of original design in a railed circular showcase of generic consoles) and the directory's totem.

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/ext/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::formatNumber;
using detail::outOfRange;
using detail::translation;

constexpr BoxUv kGrain = BoxUv::GrainMeters;
constexpr float kWoodTile = detail::kWoodMetersPerTile;

glm::mat4 rotationY(float radians) {
    return glm::rotate(glm::mat4(1.0f), radians, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 rotationX(float radians) {
    return glm::rotate(glm::mat4(1.0f), radians, glm::vec3(1.0f, 0.0f, 0.0f));
}

glm::mat4 rotationZ(float radians) {
    return glm::rotate(glm::mat4(1.0f), radians, glm::vec3(0.0f, 0.0f, 1.0f));
}

// --- column -----------------------------------------------------------------------------------------------------

constexpr std::string_view kColumn = "column";
constexpr float kColumnTaper = 0.92f;
constexpr float kEchinusWidening = 1.25f;
constexpr int kColumnSegments = 32;

Status validateColumn(const ColumnParams& p) {
    if (Status status = firstError({
            checkRange(kColumn, "radius", p.radius, 0.05f, 0.6f),
            checkRange(kColumn, "height", p.height, 1.0f, 10.0f),
        });
        !status) {
        return status;
    }
    if (!(p.plinthSize >= 2.2f * p.radius && p.plinthSize <= 1.5f)) {
        return outOfRange(kColumn, "plinthSize",
                          formatNumber(p.plinthSize) + " is outside [" + formatNumber(2.2f * p.radius) + ", 1.5]");
    }
    if (!(p.plinthHeight >= 0.05f && p.plinthHeight <= 0.25f * p.height)) {
        return outOfRange(kColumn, "plinthHeight",
                          formatNumber(p.plinthHeight) + " is outside [0.05, " + formatNumber(0.25f * p.height) + "]");
    }
    if (!(p.capitalHeight >= 0.1f && p.capitalHeight <= 0.25f * p.height)) {
        return outOfRange(kColumn, "capitalHeight",
                          formatNumber(p.capitalHeight) + " is outside [0.1, " + formatNumber(0.25f * p.height) + "]");
    }
    return ok();
}

ProceduralPrefab assembleColumn(const ColumnParams& p) {
    detail::PrefabAssembler out(kColumn, {"wood_primary", "wood_secondary", "metal_trim"});
    MeshBuilder& shaft = out.part("wood_primary");
    MeshBuilder& carved = out.part("wood_secondary");
    MeshBuilder& brass = out.part("metal_trim");
    // The plinth: a chamfered block and a thinner step on it.
    carved.chamferedBox(glm::vec3(p.plinthSize, p.plinthHeight, p.plinthSize), 0.012f,
                        translation(glm::vec3(0.0f, 0.5f * p.plinthHeight, 0.0f)), kGrain, kWoodTile);
    const float step = std::min(0.05f, 0.25f * p.plinthHeight);
    const float stepSize = 0.5f * (p.plinthSize + 2.2f * p.radius);
    carved.chamferedBox(glm::vec3(stepSize, step, stepSize), 0.006f,
                        translation(glm::vec3(0.0f, p.plinthHeight + 0.5f * step, 0.0f)), kGrain, kWoodTile);
    const float shaftBottom = p.plinthHeight + step;
    // The capital: an echinus widening from the neck, a square abacus on it.
    const float abacus = 0.35f * p.capitalHeight;
    const float echinus = p.capitalHeight - abacus;
    const float shaftTop = p.height - p.capitalHeight;
    const float topRadius = kColumnTaper * p.radius;
    shaft.frustum(p.radius, topRadius, shaftTop - shaftBottom, kColumnSegments, false, false,
                  translation(glm::vec3(0.0f, 0.5f * (shaftBottom + shaftTop), 0.0f)));
    carved.frustum(topRadius, kEchinusWidening * p.radius, echinus, kColumnSegments, true, false,
                   translation(glm::vec3(0.0f, shaftTop + 0.5f * echinus, 0.0f)));
    const float abacusSize = std::max(2.0f * kEchinusWidening * p.radius + 0.02f, 0.9f * p.plinthSize);
    carved.chamferedBox(glm::vec3(abacusSize, abacus, abacusSize), 0.008f,
                        translation(glm::vec3(0.0f, p.height - 0.5f * abacus, 0.0f)), kGrain, kWoodTile);
    // Brass: a torus at the shaft's foot and a ring at the neck.
    const float footRing = 0.18f * p.radius;
    brass.torus(p.radius + 0.4f * footRing, footRing, kColumnSegments, 12,
                translation(glm::vec3(0.0f, shaftBottom + footRing, 0.0f)));
    const float neckRing = 0.08f * p.radius;
    brass.torus(topRadius + 0.3f * neckRing, neckRing, kColumnSegments, 10,
                translation(glm::vec3(0.0f, shaftTop - 2.0f * neckRing, 0.0f)));
    return std::move(out).finish();
}

// --- railing ----------------------------------------------------------------------------------------------------

constexpr std::string_view kRailing = "railing";
constexpr float kHandrailWidth = 0.08f;
constexpr float kHandrailHeight = 0.06f;
constexpr float kPostSide = 0.09f;
constexpr float kBottomRail = 0.05f;
constexpr float kBottomRailY = 0.08f;
constexpr float kBalusterRadius = 0.011f;

Status validateRailing(const RailingParams& p) {
    return firstError({
        checkRange(kRailing, "length", p.length, 0.3f, 60.0f),
        checkRange(kRailing, "height", p.height, 0.5f, 1.5f),
        checkRange(kRailing, "postPitch", p.postPitch, 0.5f, 10.0f),
        checkRange(kRailing, "balusterPitch", p.balusterPitch, 0.05f, 0.5f),
    });
}

ProceduralPrefab assembleRailing(const RailingParams& p) {
    detail::PrefabAssembler out(kRailing, {"wood_primary", "wood_secondary", "metal"});
    MeshBuilder& wood = out.part("wood_primary");
    MeshBuilder& rail = out.part("wood_secondary");
    MeshBuilder& metal = out.part("metal");
    const float half = 0.5f * p.length;
    wood.chamferedBox(glm::vec3(p.length, kHandrailHeight, kHandrailWidth), 0.012f,
                      translation(glm::vec3(0.0f, p.height - 0.5f * kHandrailHeight, 0.0f)), kGrain, kWoodTile);
    rail.box(glm::vec3(p.length, kBottomRail, kBottomRail),
             translation(glm::vec3(0.0f, kBottomRailY + 0.5f * kBottomRail, 0.0f)), kGrain, kWoodTile);
    // Posts at both ends and evenly between them, at most postPitch apart.
    const float span = p.length - kPostSide;
    const int bays = std::max(1, static_cast<int>(std::ceil(span / p.postPitch - 1e-4f)));
    const float bay = span / static_cast<float>(bays);
    const float postHeight = p.height - kHandrailHeight;
    for (int i = 0; i <= bays; ++i) {
        const float x = -half + 0.5f * kPostSide + static_cast<float>(i) * bay;
        wood.chamferedBox(glm::vec3(kPostSide, postHeight, kPostSide), 0.008f,
                          translation(glm::vec3(x, 0.5f * postHeight, 0.0f)), kGrain, kWoodTile);
    }
    // Balusters between the posts, centred in each bay.
    const float balusterBottom = kBottomRailY + kBottomRail;
    const float balusterHeight = postHeight - balusterBottom;
    for (int i = 0; i < bays; ++i) {
        const float from = -half + kPostSide + static_cast<float>(i) * bay;
        const float clear = bay - kPostSide;
        const int count = std::max(0, static_cast<int>(std::floor(clear / p.balusterPitch)));
        for (int k = 0; k < count; ++k) {
            const float x = from + 0.5f * clear + (static_cast<float>(k) - 0.5f * static_cast<float>(count - 1)) *
                                                      p.balusterPitch;
            metal.cylinder(kBalusterRadius, balusterHeight, 8, false,
                           translation(glm::vec3(x, balusterBottom + 0.5f * balusterHeight, 0.0f)));
        }
    }
    return std::move(out).finish();
}

// --- hub_landmark -----------------------------------------------------------------------------------------------

constexpr std::string_view kLandmark = "hub_landmark";
constexpr int kDaisSegments = 96;
/// The glass wall stands this far inside the dais's rim; the ring counter this far further in, this high; the
/// consoles on it this far inside its edge.
constexpr float kWallInset = 0.06f;
constexpr float kCounterInset = 0.42f;
constexpr float kCounterHeight = 0.38f;
constexpr float kConsoleInset = 0.32f;
constexpr int kRailPosts = 16;
constexpr float kRailPostRadius = 0.018f;
constexpr float kHandrailTube = 0.024f;
/// The pedestal: its foot and top radii, the brass dish on it.
constexpr float kPedestalFoot = 0.42f;
constexpr float kPedestalTop = 0.3f;
constexpr float kDishHeight = 0.08f;
/// The globe: its axis tilt, the wireframe's meridians and parallels and their tube, the meridian ring around it.
constexpr float kGlobeTiltDeg = 23.5f;
constexpr int kMeridians = 8;
constexpr float kParallelsDeg[] = {-60.0f, -30.0f, 0.0f, 30.0f, 60.0f};
constexpr float kGlobeLineTube = 0.009f;
constexpr float kMeridianRingGap = 0.06f;
/// A generic console of original design (the showcase's proportions): body, deck, slot, ports, buttons.
constexpr glm::vec3 kConsole{0.26f, 0.055f, 0.19f};

Status validateLandmark(const HubLandmarkParams& p) {
    if (Status status = firstError({
            checkRange(kLandmark, "radius", p.radius, 1.0f, 4.0f),
            checkRange(kLandmark, "baseHeight", p.baseHeight, 0.1f, 0.6f),
            checkRange(kLandmark, "railHeight", p.railHeight, 0.5f, 1.4f),
            checkRange(kLandmark, "pedestalHeight", p.pedestalHeight, 0.4f, 3.0f),
            checkRange(kLandmark, "consoles", p.consoles, 0, 12),
        });
        !status) {
        return status;
    }
    const float maxGlobe = std::min(1.2f, 0.5f * p.radius);
    if (!(p.globeRadius >= 0.2f && p.globeRadius <= maxGlobe)) {
        return outOfRange(kLandmark, "globeRadius",
                          formatNumber(p.globeRadius) + " is outside [0.2, " + formatNumber(maxGlobe) + "]");
    }
    return ok();
}

/// A generic console standing on y = 0 of `at`, its front towards +Z.
void addConsole(detail::PrefabAssembler& out, const glm::mat4& at) {
    out.part("plastic_body")
        .chamferedBox(kConsole, 0.008f, at * translation(glm::vec3(0.0f, 0.5f * kConsole.y, 0.0f)))
        .chamferedBox(glm::vec3(0.15f, 0.025f, 0.1f), 0.006f,
                      at * translation(glm::vec3(0.0f, kConsole.y + 0.0125f, -0.035f)));
    MeshBuilder& dark = out.part("plastic_dark");
    dark.box(glm::vec3(0.11f, 0.004f, 0.022f), at * translation(glm::vec3(0.0f, kConsole.y + 0.026f, -0.035f)));
    for (const float x : {-0.05f, 0.05f}) {
        dark.box(glm::vec3(0.03f, 0.016f, 0.004f), at * translation(glm::vec3(x, 0.022f, 0.5f * kConsole.z + 0.0015f)));
    }
    for (const float x : {-0.095f, -0.07f}) {
        out.part("plastic_accent")
            .box(glm::vec3(0.018f, 0.006f, 0.014f), at * translation(glm::vec3(x, kConsole.y + 0.003f, 0.05f)));
    }
}

ProceduralPrefab assembleLandmark(const HubLandmarkParams& p) {
    detail::PrefabAssembler out(kLandmark, {"wood_primary", "wood_secondary", "metal_trim", "glass", "fabric",
                                            "plastic_body", "plastic_dark", "plastic_accent", "globe", "globe_core"});
    MeshBuilder& wood = out.part("wood_primary");
    MeshBuilder& counter = out.part("wood_secondary");
    MeshBuilder& brass = out.part("metal_trim");
    MeshBuilder& glass = out.part("glass");
    MeshBuilder& globe = out.part("globe");

    // The dais and its brass rim.
    wood.cylinder(p.radius, p.baseHeight, kDaisSegments, true, translation(glm::vec3(0.0f, 0.5f * p.baseHeight, 0.0f)));
    brass.torus(p.radius - 0.012f, 0.012f, kDaisSegments, 8, translation(glm::vec3(0.0f, p.baseHeight, 0.0f)));
    // The railed circular showcase: a glass wall between brass posts under a round handrail.
    const float wallRadius = p.radius - kWallInset;
    const float glassHeight = p.railHeight - 0.08f;
    glass.frustum(wallRadius, wallRadius, glassHeight, kDaisSegments, false, true,
                  translation(glm::vec3(0.0f, p.baseHeight + 0.04f + 0.5f * glassHeight, 0.0f)));
    brass.torus(wallRadius, kHandrailTube, kDaisSegments, 12,
                translation(glm::vec3(0.0f, p.baseHeight + p.railHeight, 0.0f)));
    brass.torus(wallRadius, 0.6f * kHandrailTube, kDaisSegments, 8,
                translation(glm::vec3(0.0f, p.baseHeight + 0.04f, 0.0f)));
    for (int i = 0; i < kRailPosts; ++i) {
        const float angle = core::kTwoPi * static_cast<float>(i) / static_cast<float>(kRailPosts);
        const glm::vec3 at(wallRadius * std::sin(angle), p.baseHeight + 0.5f * p.railHeight,
                           wallRadius * std::cos(angle));
        brass.cylinder(kRailPostRadius, p.railHeight, 10, true, translation(at));
    }
    // The ring counter inside, its baize top, the consoles on it facing out.
    const float counterRadius = p.radius - kCounterInset;
    const float counterTop = p.baseHeight + kCounterHeight;
    counter.cylinder(counterRadius, kCounterHeight, kDaisSegments, false,
                     translation(glm::vec3(0.0f, p.baseHeight + 0.5f * kCounterHeight, 0.0f)));
    out.part("fabric").cylinder(counterRadius + 0.005f, 0.012f, kDaisSegments, true,
                                translation(glm::vec3(0.0f, counterTop + 0.006f, 0.0f)));
    const float consoleRadius = counterRadius - kConsoleInset;
    for (int i = 0; i < p.consoles; ++i) {
        // Half a step off the axes, so that none stands straight before the pedestal seen from the hub's ends.
        const float angle = core::kTwoPi * (static_cast<float>(i) + 0.5f) / static_cast<float>(p.consoles);
        const glm::mat4 at = translation(glm::vec3(consoleRadius * std::sin(angle), counterTop + 0.012f,
                                                   consoleRadius * std::cos(angle))) *
                             rotationY(angle);
        addConsole(out, at);
    }
    // The pedestal, its brass rings and dish.
    wood.frustum(kPedestalFoot, kPedestalTop, p.pedestalHeight, 48, false, false,
                 translation(glm::vec3(0.0f, counterTop + 0.5f * p.pedestalHeight, 0.0f)));
    const float pedestalTop = counterTop + p.pedestalHeight;
    brass.torus(kPedestalFoot + 0.01f, 0.02f, 48, 10, translation(glm::vec3(0.0f, counterTop + 0.03f, 0.0f)));
    brass.torus(kPedestalTop + 0.008f, 0.016f, 48, 10, translation(glm::vec3(0.0f, pedestalTop - 0.04f, 0.0f)));
    brass.frustum(0.16f, 0.36f, kDishHeight, 48, true, false,
                  translation(glm::vec3(0.0f, pedestalTop + 0.5f * kDishHeight, 0.0f)));

    // The globe: a wireframe of meridians and parallels about a tilted axis around a glowing core, held by a brass
    // meridian ring on a short stem.
    const float g = p.globeRadius;
    const glm::vec3 centre(0.0f, pedestalTop + kDishHeight + kMeridianRingGap + 0.05f + g, 0.0f);
    const glm::mat4 globeFrame = translation(centre) * rotationZ(core::degreesToRadians(kGlobeTiltDeg));
    for (int i = 0; i < kMeridians; ++i) {
        const float turn = core::kPi * static_cast<float>(i) / static_cast<float>(kMeridians);
        globe.torus(g, kGlobeLineTube, 64, 6, globeFrame * rotationY(turn) * rotationX(0.5f * core::kPi));
    }
    for (const float latitude : kParallelsDeg) {
        const float phi = core::degreesToRadians(latitude);
        globe.torus(g * std::cos(phi), kGlobeLineTube, 64, 6,
                    globeFrame * translation(glm::vec3(0.0f, g * std::sin(phi), 0.0f)));
    }
    out.part("globe_core").uvSphere(0.94f * g, 48, 24, translation(centre));
    const float ring = g + kMeridianRingGap;
    brass.torus(ring, 0.014f, 64, 8, globeFrame * rotationX(0.5f * core::kPi));
    brass.cylinder(0.01f, 2.0f * ring + 0.08f, 10, true, globeFrame);
    brass.cylinder(0.03f, centre.y - ring - (pedestalTop + kDishHeight) + 0.01f, 16, false,
                   translation(glm::vec3(0.0f, 0.5f * (centre.y - ring + pedestalTop + kDishHeight), 0.0f)));
    out.addSocket("Globe", SocketKind::Anchor, translation(centre));
    return std::move(out).finish();
}

// --- info_totem -------------------------------------------------------------------------------------------------

constexpr std::string_view kTotem = "info_totem";
constexpr float kTotemPlinth = 0.08f;
constexpr float kTotemHead = 0.16f;
/// The body stands this far inside the plinth on every side, the head this far.
constexpr float kTotemSetBack = 0.03f;
constexpr float kTotemHeadInset = 0.01f;
constexpr float kBezel = 0.022f;
constexpr float kScreenCentreFromTop = 0.62f;

Status validateTotem(const InfoTotemParams& p) {
    if (Status status = firstError({
            checkRange(kTotem, "width", p.width, 0.3f, 1.2f),
            checkRange(kTotem, "height", p.height, 1.0f, 2.5f),
            checkRange(kTotem, "depth", p.depth, 0.15f, 0.8f),
        });
        !status) {
        return status;
    }
    if (!(p.screenWidth >= 0.1f && p.screenWidth <= p.width - 0.12f)) {
        return outOfRange(kTotem, "screenWidth",
                          formatNumber(p.screenWidth) + " is outside [0.1, " + formatNumber(p.width - 0.12f) + "]");
    }
    if (!(p.screenHeight >= 0.1f && p.screenHeight <= p.height - 0.6f)) {
        return outOfRange(kTotem, "screenHeight",
                          formatNumber(p.screenHeight) + " is outside [0.1, " + formatNumber(p.height - 0.6f) + "]");
    }
    return ok();
}

ProceduralPrefab assembleTotem(const InfoTotemParams& p) {
    detail::PrefabAssembler out(kTotem, {"wood_primary", "wood_secondary", "metal_trim", "screen", "sign"});
    MeshBuilder& body = out.part("wood_primary");
    MeshBuilder& dark = out.part("wood_secondary");
    MeshBuilder& brass = out.part("metal_trim");
    // A dark plinth (the whole footprint), the body set back on it, a head under a brass band.
    dark.chamferedBox(glm::vec3(p.width, kTotemPlinth, p.depth), 0.01f,
                      translation(glm::vec3(0.0f, 0.5f * kTotemPlinth, 0.0f)), kGrain, kWoodTile);
    const float bodyTop = p.height - kTotemHead;
    const float bodyDepth = p.depth - 2.0f * kTotemSetBack;
    body.chamferedBox(glm::vec3(p.width - 2.0f * kTotemSetBack, bodyTop - kTotemPlinth, bodyDepth), 0.015f,
                      translation(glm::vec3(0.0f, 0.5f * (kTotemPlinth + bodyTop), 0.0f)), kGrain, kWoodTile);
    const float headDepth = p.depth - 2.0f * kTotemHeadInset;
    dark.chamferedBox(glm::vec3(p.width - 2.0f * kTotemHeadInset, kTotemHead, headDepth), 0.012f,
                      translation(glm::vec3(0.0f, p.height - 0.5f * kTotemHead, 0.0f)), kGrain, kWoodTile);
    const float bandHeight = 0.6f * kTotemHead;
    const float bandY = p.height - 0.5f * kTotemHead;
    const float headFront = 0.5f * headDepth;
    brass.box(glm::vec3(p.width - 0.06f, bandHeight, 0.004f), translation(glm::vec3(0.0f, bandY, headFront + 0.002f)));
    out.part("sign").quad(glm::vec2(p.width - 0.1f, bandHeight - 0.02f),
                          translation(glm::vec3(0.0f, bandY, headFront + 0.004f + detail::kDecalOffset)));
    // The screen in a brass bezel on the body's front.
    const float bodyFront = 0.5f * bodyDepth;
    const float screenY = std::max(kTotemPlinth + 0.5f * p.screenHeight + kBezel + 0.05f,
                                   p.height - kScreenCentreFromTop * std::min(1.0f, p.height / 1.75f));
    brass.box(glm::vec3(p.screenWidth + 2.0f * kBezel, p.screenHeight + 2.0f * kBezel, 0.008f),
              translation(glm::vec3(0.0f, screenY, bodyFront + 0.004f)));
    const glm::mat4 screenFrame = translation(glm::vec3(0.0f, screenY, bodyFront + 0.008f + detail::kDecalOffset));
    out.part("screen").quad(glm::vec2(p.screenWidth, p.screenHeight), screenFrame);
    out.addSocket("Screen", SocketKind::Anchor, screenFrame);
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildColumn(const ColumnParams& params) {
    if (Status status = validateColumn(params); !status) {
        return propagate(status.error());
    }
    return assembleColumn(params);
}

Expected<ProceduralPrefab> buildRailing(const RailingParams& params) {
    if (Status status = validateRailing(params); !status) {
        return propagate(status.error());
    }
    return assembleRailing(params);
}

Expected<ProceduralPrefab> buildHubLandmark(const HubLandmarkParams& params) {
    if (Status status = validateLandmark(params); !status) {
        return propagate(status.error());
    }
    return assembleLandmark(params);
}

Expected<ProceduralPrefab> buildInfoTotem(const InfoTotemParams& params) {
    if (Status status = validateTotem(params); !status) {
        return propagate(status.error());
    }
    return assembleTotem(params);
}

} // namespace rv::geometry
