// I6 step 6's arcade cabinets (ADR-0014 §6, style §5.4's `CabinetGeneric`): a row of generic upright cabinets whose
// screens are the arcade room's face-out slots, or one larger cabinet whose screen carries the room's video (the hero
// cabinet). An original design: no brand, logo or artwork; the marquee is a lit face for a picture of the scene's own.

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
using detail::formatNumber;
using detail::leaningBack;
using detail::outOfRange;
using detail::translation;

constexpr std::string_view kKind = "arcade_cabinet";

constexpr float kSide = 0.018f;     ///< the side panels' thickness
constexpr float kBack = 0.015f;     ///< the back panel's
constexpr float kCap = 0.03f;       ///< the top cap's height
constexpr float kFront = 0.018f;    ///< the lower front's and the toe's thickness
constexpr float kToeHeight = 0.09f; ///< the recessed toe under the lower front
constexpr float kToeRecess = 0.03f;
constexpr float kMolding = 0.005f;      ///< the T-moulding's depth in front of a side panel's edge
constexpr float kPanelThick = 0.06f;    ///< the control panel, under its top
constexpr float kPlate = 0.015f;        ///< the bezel plate's thickness
constexpr float kBelowScreen = 0.05f;   ///< the bezel plate below the screen's opening
constexpr float kAboveScreen = 0.03f;   ///< at least this much plate above it (the speaker's band)
constexpr float kScreenMargin = 0.02f;  ///< at least this much plate either side of the opening
/// The screen item's face stands this far behind the bezel's face, so that its slight turn (a face-out placement's
/// offset) never brings an edge through the plate.
constexpr float kScreenRecess = 0.006f;
constexpr float kChamfer = 0.003f;
/// A side panel carrying side art stands this far in from the row's side, the art half-way (I6 step 6's review).
constexpr float kArtInset = 0.002f;

// The stepped profile, as shares of the depth from the back.
constexpr float kLowerFrontShare = 0.76f;
constexpr float kPanelBackShare = 0.42f;
constexpr float kBezelShare = 0.64f;
constexpr float kMarqueeShare = 0.9f;
constexpr float kCapShare = 0.93f;

/// One cabinet's profile and screen (shared by every cabinet of the row).
struct Profile {
    float back = 0.0f;         ///< z of the back face
    float lowerFront = 0.0f;   ///< z of the lower body's front face
    float panelBack = 0.0f;    ///< the control panel's back edge
    float panelFront = 0.0f;   ///< its front edge
    float bezel = 0.0f;        ///< the bezel plate's face at its bottom edge
    float marqueeFront = 0.0f; ///< the marquee's lit face
    float capFront = 0.0f;
    float panelBottom = 0.0f;  ///< the control panel's fascia's lower edge
    float marqueeBottom = 0.0f;
    float marqueeTop = 0.0f;
    float tilt = 0.0f;         ///< radians
    float plateLength = 0.0f;  ///< along the leaning plate, from the control panel to the marquee
    float plateTopZ = 0.0f;    ///< z of the plate's face at its top edge
    float innerWidth = 0.0f;
    float rowWidth = 0.0f;
};

Profile profileOf(const ArcadeCabinetParams& p) {
    Profile f;
    f.back = -0.5f * p.depth;
    f.lowerFront = f.back + kLowerFrontShare * p.depth;
    f.panelBack = f.back + kPanelBackShare * p.depth;
    f.panelFront = 0.5f * p.depth;
    f.bezel = f.back + kBezelShare * p.depth;
    f.marqueeFront = f.back + kMarqueeShare * p.depth;
    f.capFront = f.back + kCapShare * p.depth;
    f.panelBottom = p.panelHeight - kPanelThick;
    f.marqueeTop = p.height - kCap;
    f.marqueeBottom = f.marqueeTop - p.marqueeHeight;
    f.tilt = core::degreesToRadians(p.screenTiltDeg);
    f.plateLength = (f.marqueeBottom - p.panelHeight) / std::cos(f.tilt);
    f.plateTopZ = f.bezel - f.plateLength * std::sin(f.tilt);
    f.innerWidth = p.width - 2.0f * kSide;
    f.rowWidth = static_cast<float>(p.count) * p.width + static_cast<float>(p.count - 1) * p.gap;
    return f;
}

/// The centre of cabinet `i` along X.
float cabinetX(const ArcadeCabinetParams& p, const Profile& f, int i) {
    return -0.5f * f.rowWidth + 0.5f * p.width + static_cast<float>(i) * (p.width + p.gap);
}

/// The leaning frame of cabinet `i`'s bezel plate: its origin on the plate's face at its bottom edge, +Y up the plate,
/// +Z out of it.
glm::mat4 plateFrame(const ArcadeCabinetParams& p, const Profile& f, int i) {
    return leaningBack(f.tilt, glm::vec3(cabinetX(p, f, i), p.panelHeight, f.bezel));
}

Status validate(const ArcadeCabinetParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "width", p.width, 0.3f, 1.2f),
            checkRange(kKind, "height", p.height, 1.2f, 2.4f),
            checkRange(kKind, "depth", p.depth, 0.3f, 1.2f),
            checkRange(kKind, "count", p.count, 1, 4),
            checkRange(kKind, "gap", p.gap, 0.0f, 0.5f),
            checkRange(kKind, "panelHeight", p.panelHeight, 0.6f, 1.1f),
            checkRange(kKind, "marqueeHeight", p.marqueeHeight, 0.08f, 0.4f),
            checkRange(kKind, "screenSize", p.screenSize, glm::vec2(0.1f), glm::vec2(1.0f)),
            checkRange(kKind, "screenTiltDeg", p.screenTiltDeg, 0.0f, 25.0f),
            checkRange(kKind, "slotDepth", p.slotDepth, 0.002f, 0.05f),
        });
        !status) {
        return status;
    }
    const Profile f = profileOf(p);
    const float widest = f.innerWidth - 2.0f * kScreenMargin;
    if (p.screenSize.x > widest + 1e-5f) {
        return outOfRange(kKind, "screenSize",
                          "the screen's width " + formatNumber(p.screenSize.x) + " m is wider than the cabinet leaves (" +
                              formatNumber(widest) + " m)");
    }
    const float tallest = f.plateLength - kBelowScreen - kAboveScreen;
    if (p.screenSize.y > tallest + 1e-5f) {
        return outOfRange(kKind, "screenSize",
                          "the screen's height " + formatNumber(p.screenSize.y) +
                              " m is taller than the control panel and the marquee leave (" +
                              formatNumber(std::max(tallest, 0.0f)) + " m)");
    }
    if (f.plateTopZ < f.back + kBack + 0.02f) {
        return outOfRange(kKind, "screenTiltDeg",
                          "the screen leans back through the cabinet's back (" + formatNumber(p.screenTiltDeg) +
                              " degrees in a cabinet " + formatNumber(p.depth) + " m deep)");
    }
    return ok();
}

/// A box from its corners.
void boxBetween(MeshBuilder& builder, const glm::vec3& low, const glm::vec3& high, FaceMask faces = FaceMask::All) {
    builder.box(high - low, translation(0.5f * (low + high)), BoxUv::PerFace, 1.0f, faces);
}

void addCabinet(detail::PrefabAssembler& out, const ArcadeCabinetParams& p, const Profile& f, int i) {
    const bool alt = i % 2 == 1;
    MeshBuilder& body = out.part(alt ? "body_alt" : "body");
    MeshBuilder& front = out.part("front");
    MeshBuilder& molding = out.part(alt ? "molding_alt" : "molding");
    MeshBuilder& dark = out.part("plastic_dark");
    MeshBuilder& metal = out.part("metal_trim");
    MeshBuilder& accent = out.part("plastic_accent");
    const float x = cabinetX(p, f, i);
    const float half = 0.5f * p.width;
    const float innerHalf = 0.5f * f.innerWidth;

    // The side panels: the lower body, the control panel's band, the screen's and the marquee's (under the cap), each
    // as deep as the profile there, a moulding along each front edge.
    const float screenSideFront = f.bezel + 0.02f;
    // A side carrying the row's side art stands kArtInset inside the row's width, the art over it (the bounds as
    // without).
    const auto artSide = [&](float side) {
        return p.sideArt && ((side < 0.0f && i == 0) || (side > 0.0f && i == p.count - 1));
    };
    for (const float side : {-1.0f, 1.0f}) {
        const float inset = artSide(side) ? kArtInset : 0.0f;
        const float x0 = x + side * half - (side > 0.0f ? kSide : 0.0f) + (side < 0.0f ? inset : 0.0f);
        const float x1 = x0 + kSide - inset;
        boxBetween(body, glm::vec3(x0, 0.0f, f.back), glm::vec3(x1, f.panelBottom, f.lowerFront));
        boxBetween(body, glm::vec3(x0, f.panelBottom, f.back), glm::vec3(x1, p.panelHeight + 0.02f, f.panelFront));
        boxBetween(body, glm::vec3(x0, p.panelHeight + 0.02f, f.back), glm::vec3(x1, f.marqueeBottom, screenSideFront));
        boxBetween(body, glm::vec3(x0, f.marqueeBottom, f.back), glm::vec3(x1, f.marqueeTop, f.capFront));
        const float m0 = x0;
        const float m1 = x1;
        boxBetween(molding, glm::vec3(m0, 0.0f, f.lowerFront), glm::vec3(m1, f.panelBottom, f.lowerFront + kMolding),
                   FaceMask::All & ~FaceMask::NegY);
        boxBetween(molding, glm::vec3(m0, p.panelHeight + 0.02f, screenSideFront),
                   glm::vec3(m1, f.marqueeBottom, screenSideFront + kMolding));
        boxBetween(molding, glm::vec3(m0, f.marqueeBottom, f.capFront),
                   glm::vec3(m1, f.marqueeTop, f.capFront + kMolding));
    }

    // The row's side art (I6 step 6's review): a printed panel on the outer side of its first and last cabinets, from
    // the toe to the marquee, the stepped profile's outline less a margin; one picture across both quads.
    if (artSide(-1.0f) || artSide(1.0f)) {
        constexpr float kArtMargin = 0.015f;
        const float z0 = f.back + kArtMargin;
        const float zLow = f.lowerFront - kArtMargin;
        const float zHigh = screenSideFront - kArtMargin;
        const float y0 = kToeHeight;
        const float yBand = p.panelHeight + 0.02f;
        const float y1 = f.marqueeBottom - kArtMargin;
        MeshBuilder& art = out.part("side_art");
        for (const float side : {-1.0f, 1.0f}) {
            if (!artSide(side)) {
                continue;
            }
            // u along the panel (from the back on the left side, from the front on the right: each read from
            // outside), v down it.
            const auto u = [&](float z) { return side < 0.0f ? (z - z0) / (zLow - z0) : (zLow - z) / (zLow - z0); };
            const auto v = [&](float y) { return (y1 - y) / (y1 - y0); };
            const float faceX = x + side * (half - 0.5f * kArtInset);
            const auto quadBetween = [&](float za, float zb, float ya, float yb) {
                glm::mat4 frame = glm::rotate(glm::mat4(1.0f), side * glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
                frame[3] = glm::vec4(faceX, 0.5f * (ya + yb), 0.5f * (za + zb), 1.0f);
                // The quad's left edge (-X before the turn) is at the back on the left side, the front on the right.
                const float left = side < 0.0f ? za : zb;
                const float right = side < 0.0f ? zb : za;
                art.quad(glm::vec2(zb - za, yb - ya), frame, glm::vec4(u(left), v(yb), u(right), v(ya)));
            };
            quadBetween(z0, zLow, y0, yBand);
            quadBetween(z0, zHigh, yBand, y1);
        }
    }

    // The back, the top cap, the lower front over a recessed toe.
    boxBetween(front, glm::vec3(x - innerHalf, 0.0f, f.back), glm::vec3(x + innerHalf, f.marqueeTop, f.back + kBack));
    front.chamferedBox(glm::vec3(p.width, kCap, f.capFront - f.back), kChamfer,
                       translation(glm::vec3(x, p.height - 0.5f * kCap, 0.5f * (f.back + f.capFront))),
                       BoxUv::PerFace, 1.0f, FaceMask::PosY);
    boxBetween(front, glm::vec3(x - innerHalf, kToeHeight, f.lowerFront - kFront),
               glm::vec3(x + innerHalf, f.panelBottom, f.lowerFront));
    boxBetween(front, glm::vec3(x - innerHalf, 0.0f, f.lowerFront - kToeRecess - kFront),
               glm::vec3(x + innerHalf, kToeHeight, f.lowerFront - kToeRecess), FaceMask::All & ~FaceMask::NegY);

    // The coin door, with two coin slots.
    const float doorWidth = std::min(0.16f, 0.45f * f.innerWidth);
    const float doorHeight = std::min(0.2f, 0.4f * (f.panelBottom - kToeHeight));
    const float doorY = kToeHeight + 0.55f * (f.panelBottom - kToeHeight);
    dark.box(glm::vec3(doorWidth, doorHeight, 0.006f), translation(glm::vec3(x, doorY, f.lowerFront + 0.003f)));
    for (const float slot : {-0.3f, 0.3f}) {
        metal.box(glm::vec3(0.022f, 0.045f, 0.006f),
                  translation(glm::vec3(x + slot * doorWidth, doorY + 0.2f * doorHeight, f.lowerFront + 0.007f)));
    }

    // The control panel: its deck, a joystick and three buttons.
    dark.chamferedBox(glm::vec3(f.innerWidth, kPanelThick, f.panelFront - f.panelBack), kChamfer,
                      translation(glm::vec3(x, p.panelHeight - 0.5f * kPanelThick, 0.5f * (f.panelBack + f.panelFront))),
                      BoxUv::PerFace, 1.0f, FaceMask::PosY | FaceMask::PosZ);
    const float deckZ = f.panelBack + 0.55f * (f.panelFront - f.panelBack);
    const float scale = f.innerWidth / 0.384f;
    const float stickX = x - 0.25f * f.innerWidth;
    metal.cylinder(0.005f, 0.05f, 12, true, translation(glm::vec3(stickX, p.panelHeight + 0.025f, deckZ)));
    accent.uvSphere(0.017f, 16, 8, translation(glm::vec3(stickX, p.panelHeight + 0.055f, deckZ)));
    for (const float button : {-0.01f, 0.035f, 0.08f}) {
        accent.cylinder(0.012f, 0.012f, 16, true,
                        translation(glm::vec3(x + button * scale, p.panelHeight + 0.006f, deckZ - 0.01f)));
    }

    // The bezel plate leaning back from the control panel to the marquee, around the screen's opening.
    const glm::mat4 plate = plateFrame(p, f, i);
    const glm::vec2 screen = p.screenSize;
    const float top = kBelowScreen + screen.y;
    const auto plateBox = [&](float x0, float x1, float y0, float y1) {
        dark.box(glm::vec3(x1 - x0, y1 - y0, kPlate),
                 plate * translation(glm::vec3(0.5f * (x0 + x1), 0.5f * (y0 + y1), -0.5f * kPlate)));
    };
    plateBox(-innerHalf, innerHalf, 0.0f, kBelowScreen);
    plateBox(-innerHalf, innerHalf, top, f.plateLength);
    plateBox(-innerHalf, -0.5f * screen.x, kBelowScreen, top);
    plateBox(0.5f * screen.x, innerHalf, kBelowScreen, top);

    // The screen: a slot for the game's (the collection's cabinets) or a screen for the room's video (the hero's).
    const float screenY = kBelowScreen + 0.5f * screen.y;
    if (p.slots) {
        out.part("screen_back")
            .quad(screen, plate * translation(glm::vec3(0.0f, screenY, -kScreenRecess - p.slotDepth - 0.002f)));
        out.addSocket("Slot_0_" + std::to_string(i), SocketKind::ShelfSlot,
                      plate * translation(glm::vec3(0.0f, kBelowScreen, -kScreenRecess - 0.5f * p.slotDepth)),
                      core::Aabb::fromMinMax(glm::vec3(-0.5f * screen.x, 0.0f, -0.5f * p.slotDepth),
                                             glm::vec3(0.5f * screen.x, screen.y, 0.5f * p.slotDepth)));
    } else {
        const glm::mat4 surface = plate * translation(glm::vec3(0.0f, screenY, -kScreenRecess));
        out.part("screen").quad(screen, surface);
        if (i == 0) {
            out.addSocket("ScreenSurface", SocketKind::ScreenSurface, surface,
                          core::Aabb::fromMinMax(glm::vec3(-0.5f * screen.x, -0.5f * screen.y, 0.0f),
                                                 glm::vec3(0.5f * screen.x, 0.5f * screen.y, 0.001f)),
                          "screen");
        }
    }

    // The marquee: its lit face under the cap, a hood over its lower edge.
    boxBetween(front, glm::vec3(x - innerHalf, f.marqueeBottom, f.plateTopZ - kPlate),
               glm::vec3(x + innerHalf, f.marqueeTop, f.marqueeFront - 0.004f), FaceMask::All & ~FaceMask::PosZ);
    out.part(alt ? "marquee_alt" : "marquee")
        .quad(glm::vec2(f.innerWidth, p.marqueeHeight),
              translation(glm::vec3(x, 0.5f * (f.marqueeBottom + f.marqueeTop), f.marqueeFront - 0.004f)));
    boxBetween(front, glm::vec3(x - innerHalf, f.marqueeBottom - 0.012f, f.marqueeFront - 0.05f),
               glm::vec3(x + innerHalf, f.marqueeBottom, f.marqueeFront + 0.004f));
}

ProceduralPrefab assemble(const ArcadeCabinetParams& p) {
    const Profile f = profileOf(p);
    detail::PrefabAssembler out(kKind, {"body", "body_alt", "front", "molding", "molding_alt", "plastic_dark",
                                        "metal_trim", "plastic_accent", "marquee", "marquee_alt",
                                        p.slots ? "screen_back" : "screen"});
    if (p.slots) {
        // The level's open volume before its slots: the box the row's screens span.
        core::Aabb screens;
        for (int i = 0; i < p.count; ++i) {
            const glm::mat4 plate = plateFrame(p, f, i);
            for (const float x : {-0.5f * p.screenSize.x, 0.5f * p.screenSize.x}) {
                for (const float y : {kBelowScreen, kBelowScreen + p.screenSize.y}) {
                    for (const float z : {-kScreenRecess - p.slotDepth, 0.0f}) {
                        screens.expand(glm::vec3(plate * glm::vec4(x, y, z, 1.0f)));
                    }
                }
            }
        }
        out.addSocket("Level_0", SocketKind::Anchor, glm::mat4(1.0f), screens);
    }
    for (int i = 0; i < p.count; ++i) {
        addCabinet(out, p, f, i);
    }
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildArcadeCabinet(const ArcadeCabinetParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
