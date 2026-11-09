// I12 step 3's hero corner set (ADR-0021 §1): the pieces that make the TV corner a lived-in place: a floor speaker, a
// media rack with its components, a game pad with its cable, a low table, a cushion and a game's box lying open. Every
// one of original design.

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::outOfRange;
using detail::translation;

constexpr BoxUv kGrain = BoxUv::GrainMeters;
constexpr float kWoodTile = detail::kWoodMetersPerTile;

/// A frame at `position` whose +Y points along the unit vector `direction` (a cylinder laid along a segment).
glm::mat4 alongY(const glm::vec3& position, const glm::vec3& direction) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::mat4 at = translation(position);
    const float cosine = glm::dot(up, direction);
    if (cosine > 0.9999f) {
        return at;
    }
    if (cosine < -0.9999f) {
        return glm::rotate(at, core::kPi, glm::vec3(1.0f, 0.0f, 0.0f));
    }
    const glm::vec3 axis = glm::normalize(glm::cross(up, direction));
    return glm::rotate(at, std::acos(std::clamp(cosine, -1.0f, 1.0f)), axis);
}

/// A frame at `position` turning a torus (around +Y) to face +Z.
glm::mat4 facingFront(const glm::vec3& position) {
    return glm::rotate(translation(position), 0.5f * core::kPi, glm::vec3(1.0f, 0.0f, 0.0f));
}

// --- floor_speaker -------------------------------------------------------------------------------------------------

constexpr std::string_view kSpeaker = "floor_speaker";
constexpr float kGrilleDepth = 0.006f;
constexpr float kRingDepth = 0.008f;

Status validate(const FloorSpeakerParams& p) {
    return firstError({checkRange(kSpeaker, "width", p.width, 0.12f, 0.5f),
                       checkRange(kSpeaker, "height", p.height, 0.3f, 1.4f),
                       checkRange(kSpeaker, "depth", p.depth, 0.15f, 0.5f)});
}

ProceduralPrefab assemble(const FloorSpeakerParams& p) {
    detail::PrefabAssembler out(kSpeaker, {"cabinet", "grille", "trim", "led"});
    const float front = 0.5f * p.depth;
    const float cabinetDepth = p.depth - kRingDepth - kGrilleDepth;
    out.part("cabinet").chamferedBox(
        glm::vec3(p.width, p.height, cabinetDepth), 0.006f,
        translation(glm::vec3(0.0f, 0.5f * p.height, -0.5f * p.depth + 0.5f * cabinetDepth)), kGrain, kWoodTile);
    // The grille over the front, short of its edges.
    const float grilleFront = front - kRingDepth;
    const glm::vec2 grille(p.width - 0.02f, p.height - 0.06f);
    out.part("grille").box(glm::vec3(grille.x, grille.y, kGrilleDepth),
                           translation(glm::vec3(0.0f, 0.04f + 0.5f * grille.y, grilleFront - 0.5f * kGrilleDepth)));
    // The drivers' trim rings: a woofer low, a tweeter high.
    const float minor = 0.5f * kRingDepth;
    const float woofer = 0.36f * p.width;
    const float tweeter = 0.14f * p.width;
    MeshBuilder& trim = out.part("trim");
    trim.torus(woofer - minor, minor, 32, 8, facingFront(glm::vec3(0.0f, 0.04f + 0.32f * grille.y, front - minor)));
    trim.torus(tweeter - minor, minor, 24, 8, facingFront(glm::vec3(0.0f, 0.04f + 0.72f * grille.y, front - minor)));
    // The power LED, at the grille's top right.
    out.part("led").box(
        glm::vec3(0.008f, 0.008f, 0.004f),
        translation(glm::vec3(0.5f * grille.x - 0.015f, 0.04f + grille.y - 0.015f, grilleFront + 0.002f)));
    return std::move(out).finish();
}

// --- media_rack ----------------------------------------------------------------------------------------------------

constexpr std::string_view kRack = "media_rack";
constexpr float kPlinth = 0.04f;
constexpr float kRackTop = 0.025f;
constexpr float kRackSide = 0.02f;
constexpr float kDeckGap = 0.01f;

float deckHeight(const MediaRackParams& p) {
    const float inside = p.height - kPlinth - kRackTop;
    return std::min(0.1f, (inside - kDeckGap * static_cast<float>(p.decks + 1)) / static_cast<float>(p.decks));
}

Status validate(const MediaRackParams& p) {
    if (Status status = firstError(
            {checkRange(kRack, "width", p.width, 0.25f, 0.8f), checkRange(kRack, "height", p.height, 0.2f, 1.2f),
             checkRange(kRack, "depth", p.depth, 0.25f, 0.6f), checkRange(kRack, "decks", p.decks, 1, 4)});
        !status) {
        return status;
    }
    if (deckHeight(p) < 0.06f) {
        return outOfRange(kRack, "decks", "the rack is too low for its components (6 cm each)");
    }
    return ok();
}

ProceduralPrefab assemble(const MediaRackParams& p) {
    detail::PrefabAssembler out(kRack, {"wood", "metal", "display", "led"});
    MeshBuilder& wood = out.part("wood");
    wood.chamferedBox(glm::vec3(p.width, kPlinth, p.depth), 0.004f, translation(glm::vec3(0.0f, 0.5f * kPlinth, 0.0f)),
                      kGrain, kWoodTile);
    wood.chamferedBox(glm::vec3(p.width, kRackTop, p.depth), 0.004f,
                      translation(glm::vec3(0.0f, p.height - 0.5f * kRackTop, 0.0f)), kGrain, kWoodTile);
    const float sideHeight = p.height - kPlinth - kRackTop;
    for (const float side : {-1.0f, 1.0f}) {
        wood.box(glm::vec3(kRackSide, sideHeight, p.depth),
                 translation(glm::vec3(side * (0.5f * p.width - 0.5f * kRackSide), kPlinth + 0.5f * sideHeight, 0.0f)),
                 kGrain, kWoodTile);
    }
    const float inner = p.width - 2.0f * kRackSide - 0.01f;
    const float deck = deckHeight(p);
    const float deckDepth = p.depth - 0.04f;
    const float deckFront = 0.5f * p.depth - 0.02f;
    MeshBuilder& metal = out.part("metal");
    MeshBuilder& display = out.part("display");
    MeshBuilder& led = out.part("led");
    for (int i = 0; i < p.decks; ++i) {
        const float y = kPlinth + kDeckGap + static_cast<float>(i) * (deck + kDeckGap) + 0.5f * deck;
        metal.chamferedBox(glm::vec3(inner, deck, deckDepth), 0.003f,
                           translation(glm::vec3(0.0f, y, deckFront - 0.5f * deckDepth)));
        display.box(glm::vec3(0.35f * inner, 0.28f * deck, 0.002f),
                    translation(glm::vec3(-0.12f * inner, y, deckFront + 0.001f)));
        for (const float x : {0.3f, 0.38f}) {
            led.box(glm::vec3(0.006f, 0.006f, 0.003f), translation(glm::vec3(x * inner, y, deckFront + 0.0015f)));
        }
    }
    out.addSocket("Top", SocketKind::Anchor, translation(glm::vec3(0.0f, p.height, 0.0f)));
    return std::move(out).finish();
}

// --- pad -----------------------------------------------------------------------------------------------------------

constexpr std::string_view kPad = "pad";
constexpr float kCableRadius = 0.0025f;
constexpr int kCableSegments = 14;

/// A pad's body: its width, height and depth by style.
glm::vec3 padBody(PadStyle style) {
    switch (style) {
    case PadStyle::Flat:
        return {0.14f, 0.025f, 0.065f};
    case PadStyle::Grip:
        return {0.15f, 0.035f, 0.1f};
    case PadStyle::Joystick:
        return {0.12f, 0.04f, 0.12f};
    }
    return {0.14f, 0.025f, 0.065f};
}

Status validate(const PadParams& p) {
    if (Status status = firstError(
            {checkRange(kPad, "cableTo", p.cableTo, glm::vec3(-3.0f, 0.0f, -3.0f), glm::vec3(3.0f, 3.0f, 3.0f)),
             checkRange(kPad, "sag", p.sag, 0.0f, 1.0f)});
        !status) {
        return status;
    }
    return ok();
}

ProceduralPrefab assemble(const PadParams& p) {
    detail::PrefabAssembler out(kPad, {"plastic_body", "plastic_dark", "cable"});
    const glm::vec3 body = padBody(p.style);
    MeshBuilder& shell = out.part("plastic_body");
    MeshBuilder& dark = out.part("plastic_dark");
    const float top = body.y;
    if (p.style == PadStyle::Grip) {
        // A middle and two grips reaching towards the player.
        shell.chamferedBox(glm::vec3(0.6f * body.x, body.y, 0.6f * body.z), 0.01f,
                           translation(glm::vec3(0.0f, 0.5f * body.y, -0.2f * body.z)));
        for (const float side : {-1.0f, 1.0f}) {
            shell.chamferedBox(glm::vec3(0.32f * body.x, body.y, body.z), 0.012f,
                               translation(glm::vec3(side * 0.34f * body.x, 0.5f * body.y, 0.0f)));
        }
    } else {
        shell.chamferedBox(body, std::min(0.012f, 0.4f * body.y), translation(glm::vec3(0.0f, 0.5f * body.y, 0.0f)));
    }
    if (p.style == PadStyle::Joystick) {
        // The stick and its ball, a fire button.
        dark.cylinder(0.006f, 0.08f, 12, true, translation(glm::vec3(0.0f, top + 0.04f, 0.0f)));
        dark.uvSphere(0.014f, 16, 8, translation(glm::vec3(0.0f, top + 0.085f, 0.0f)));
        dark.cylinder(0.011f, 0.008f, 16, true, translation(glm::vec3(0.035f, top + 0.004f, 0.035f)));
    } else {
        // The direction pad (a cross) on the left, four buttons on the right.
        const float x = 0.28f * body.x;
        dark.box(glm::vec3(0.03f, 0.004f, 0.01f), translation(glm::vec3(-x, top + 0.002f, 0.0f)));
        dark.box(glm::vec3(0.01f, 0.004f, 0.03f), translation(glm::vec3(-x, top + 0.002f, 0.0f)));
        for (const glm::vec2 button :
             {glm::vec2(0.012f, 0.0f), glm::vec2(-0.012f, 0.0f), glm::vec2(0.0f, 0.012f), glm::vec2(0.0f, -0.012f)}) {
            dark.cylinder(0.0055f, 0.004f, 12, true, translation(glm::vec3(x + button.x, top + 0.002f, button.y)));
        }
    }
    // The cable from the pad's back, sagging to its far end, never below the floor.
    const glm::vec3 from(0.0f, 0.5f * body.y, -0.5f * body.z);
    const glm::vec3 to(p.cableTo.x, std::max(p.cableTo.y, kCableRadius), p.cableTo.z);
    const float length = glm::length(to - from);
    if (length >= 0.02f) {
        MeshBuilder& cable = out.part("cable");
        glm::vec3 previous = from;
        for (int i = 1; i <= kCableSegments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kCableSegments);
            glm::vec3 point = from + t * (to - from);
            point.y = std::max(point.y - p.sag * length * 4.0f * t * (1.0f - t), kCableRadius);
            const glm::vec3 segment = point - previous;
            const float span = glm::length(segment);
            if (span > 1e-5f) {
                cable.cylinder(kCableRadius, span, 6, false, alongY(0.5f * (previous + point), segment / span));
            }
            previous = point;
        }
    }
    out.addSocket("Cable", SocketKind::Anchor, translation(to));
    return std::move(out).finish();
}

// --- low_table -----------------------------------------------------------------------------------------------------

constexpr std::string_view kTable = "low_table";
constexpr float kTableTop = 0.03f;

Status validate(const LowTableParams& p) {
    return firstError({checkRange(kTable, "width", p.width, 0.3f, 1.6f),
                       checkRange(kTable, "depth", p.depth, 0.3f, 1.0f),
                       checkRange(kTable, "height", p.height, 0.25f, 0.8f)});
}

ProceduralPrefab assemble(const LowTableParams& p) {
    detail::PrefabAssembler out(kTable, {"wood_primary", "wood_secondary"});
    MeshBuilder& top = out.part("wood_primary");
    MeshBuilder& under = out.part("wood_secondary");
    const float topY = p.height - 0.5f * kTableTop;
    if (p.shape == TableShape::Rect) {
        top.chamferedBox(glm::vec3(p.width, kTableTop, p.depth), 0.004f, translation(glm::vec3(0.0f, topY, 0.0f)),
                         kGrain, kWoodTile, FaceMask::PosY);
        const float leg = 0.04f;
        const float legHeight = p.height - kTableTop;
        for (const float x : {-1.0f, 1.0f}) {
            for (const float z : {-1.0f, 1.0f}) {
                under.box(
                    glm::vec3(leg, legHeight, leg),
                    translation(glm::vec3(x * (0.5f * p.width - leg), 0.5f * legHeight, z * (0.5f * p.depth - leg))),
                    kGrain, kWoodTile);
            }
        }
        under.box(glm::vec3(p.width - 0.12f, 0.018f, p.depth - 0.1f), translation(glm::vec3(0.0f, 0.12f, 0.0f)), kGrain,
                  kWoodTile);
    } else {
        const float radius = 0.5f * p.width;
        top.cylinder(radius, kTableTop, 40, true, translation(glm::vec3(0.0f, topY, 0.0f)));
        const float stem = p.height - kTableTop - 0.02f;
        under.cylinder(0.035f, stem, 16, false, translation(glm::vec3(0.0f, 0.02f + 0.5f * stem, 0.0f)));
        under.cylinder(std::min(0.18f, 0.4f * p.width), 0.02f, 32, true, translation(glm::vec3(0.0f, 0.01f, 0.0f)));
    }
    out.addSocket("Top", SocketKind::Anchor, translation(glm::vec3(0.0f, p.height, 0.0f)));
    return std::move(out).finish();
}

// --- cushion -------------------------------------------------------------------------------------------------------

constexpr std::string_view kCushion = "cushion";

Status validate(const CushionParams& p) {
    return firstError({checkRange(kCushion, "width", p.width, 0.2f, 0.8f),
                       checkRange(kCushion, "height", p.height, 0.05f, 0.3f),
                       checkRange(kCushion, "depth", p.depth, 0.2f, 0.8f)});
}

ProceduralPrefab assemble(const CushionParams& p) {
    detail::PrefabAssembler out(kCushion, {"fabric"});
    out.part("fabric").chamferedBox(glm::vec3(p.width, p.height, p.depth), 0.4f * p.height,
                                    translation(glm::vec3(0.0f, 0.5f * p.height, 0.0f)), BoxUv::WorldMeters,
                                    detail::kFabricMetersPerTile);
    return std::move(out).finish();
}

// --- open_box ------------------------------------------------------------------------------------------------------

constexpr std::string_view kBox = "open_box";
constexpr float kWall = 0.004f;

Status validate(const OpenBoxParams& p) {
    return firstError({checkRange(kBox, "width", p.width, 0.08f, 0.4f), checkRange(kBox, "depth", p.depth, 0.1f, 0.4f),
                       checkRange(kBox, "height", p.height, 0.01f, 0.1f)});
}

/// An open half of the box at x: its bottom and four walls `height` tall.
void openHalf(MeshBuilder& body, const OpenBoxParams& p, float x, float height) {
    body.box(glm::vec3(p.width, kWall, p.depth), translation(glm::vec3(x, 0.5f * kWall, 0.0f)));
    for (const float z : {-1.0f, 1.0f}) {
        body.box(glm::vec3(p.width, height, kWall),
                 translation(glm::vec3(x, 0.5f * height, z * 0.5f * (p.depth - kWall))));
    }
    for (const float side : {-1.0f, 1.0f}) {
        body.box(glm::vec3(kWall, height, p.depth - 2.0f * kWall),
                 translation(glm::vec3(x + side * 0.5f * (p.width - kWall), 0.5f * height, 0.0f)));
    }
}

ProceduralPrefab assemble(const OpenBoxParams& p) {
    detail::PrefabAssembler out(kBox, {"body", "paper", "disc"});
    MeshBuilder& body = out.part("body");
    const float left = -0.5f * p.width;
    const float right = 0.5f * p.width;
    // The manual in the left half.
    openHalf(body, p, left, p.height);
    out.part("paper").box(glm::vec3(p.width - 4.0f * kWall, 0.004f, p.depth - 4.0f * kWall),
                          translation(glm::vec3(left, kWall + 0.002f, 0.0f)));
    if (p.style == OpenBoxStyle::Case) {
        // The other half open beside it, its disc in it.
        openHalf(body, p, right, p.height);
        out.part("disc").cylinder(0.42f * std::min(p.width, p.depth), 0.0015f, 48, true,
                                  translation(glm::vec3(right, kWall + 0.00075f, 0.0f)));
    } else {
        // The lid lying upside down beside the tray.
        openHalf(body, p, right, 0.5f * p.height);
    }
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildFloorSpeaker(const FloorSpeakerParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

Expected<ProceduralPrefab> buildMediaRack(const MediaRackParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

Expected<ProceduralPrefab> buildPad(const PadParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

Expected<ProceduralPrefab> buildLowTable(const LowTableParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

Expected<ProceduralPrefab> buildCushion(const CushionParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

Expected<ProceduralPrefab> buildOpenBox(const OpenBoxParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
