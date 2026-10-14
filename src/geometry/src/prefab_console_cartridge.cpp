#include <algorithm>
#include <cmath>

#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::translation;

constexpr std::string_view kKind = "console_cartridge";
/// Controller port surrounds stand this far out of the front face (they define the front of the bounds).
constexpr float kPortDepth = 0.002f;
constexpr float kVentPitch = 0.0065f;
constexpr int kMaxVents = 6;

Status validate(const ConsoleCartridgeParams& p) {
    return firstError({
        checkRange(kKind, "width", p.width, 0.12f, 1.0f),
        checkRange(kKind, "height", p.height, 0.03f, 0.3f),
        checkRange(kKind, "depth", p.depth, 0.1f, 1.0f),
    });
}

ProceduralPrefab assemble(const ConsoleCartridgeParams& p) {
    detail::PrefabAssembler out(kKind, {"plastic_body", "plastic_dark", "plastic_accent"});
    MeshBuilder& body = out.part("plastic_body");
    MeshBuilder& dark = out.part("plastic_dark");
    MeshBuilder& accent = out.part("plastic_accent");

    const float k = std::clamp(p.width / 0.24f, 0.5f, 2.0f); // detail scale relative to the default size
    const float halfWidth = 0.5f * p.width;
    const float halfDepth = 0.5f * p.depth;
    const FaceMask onTop = FaceMask::All & ~FaceMask::NegY;

    // Low base; its front face sits kPortDepth behind the bounds so the port surrounds can stand out of it.
    const float baseHeight = 0.62f * p.height;
    const float baseChamfer = std::min(0.006f, 0.12f * baseHeight);
    const float baseFront = halfDepth - kPortDepth;
    body.chamferedBox(glm::vec3(p.width, baseHeight, p.depth - kPortDepth), baseChamfer,
                      translation(glm::vec3(0.0f, 0.5f * baseHeight, -0.5f * kPortDepth)));

    // Raised deck towards the back, carrying the cartridge slot.
    const float ringHeight = std::min(0.004f, 0.25f * (p.height - baseHeight));
    const float deckTop = p.height - ringHeight;
    const float deckHeight = deckTop - baseHeight;
    const float deckWidth = 0.75f * p.width;
    const float deckDepth = 0.52f * p.depth;
    const float deckZ = -halfDepth + 0.06f * p.depth + 0.5f * deckDepth;
    const float deckChamfer = std::min(0.005f, 0.25f * deckHeight);
    body.chamferedBox(glm::vec3(deckWidth, deckHeight, deckDepth), deckChamfer,
                      translation(glm::vec3(0.0f, baseHeight + 0.5f * deckHeight, deckZ)));

    // Slot opening framed by a raised ring on the flat top of the deck; the dark flap plate lies just above
    // the deck top inside the ring. At the default size the opening takes a default cartridge (0.125 m wide).
    const float ringOuterWidth = deckWidth - 2.0f * deckChamfer - 0.002f;
    const float ringOuterDepth = deckDepth - 2.0f * deckChamfer - 0.002f;
    const float slotWidth = std::min(0.15f, 0.82f * ringOuterWidth);
    const float slotDepth = std::min(0.045f, 0.5f * ringOuterDepth);
    const float ring = std::min({0.008f, 0.5f * (ringOuterWidth - slotWidth), 0.5f * (ringOuterDepth - slotDepth)});
    const float ringY = deckTop + 0.5f * ringHeight;
    const float halfSlotWidth = 0.5f * slotWidth;
    const float halfSlotDepth = 0.5f * slotDepth;
    body.box(glm::vec3(slotWidth + 2.0f * ring, ringHeight, ring),
             translation(glm::vec3(0.0f, ringY, deckZ - halfSlotDepth - 0.5f * ring)), BoxUv::PerFace, 1.0f, onTop);
    body.box(glm::vec3(slotWidth + 2.0f * ring, ringHeight, ring),
             translation(glm::vec3(0.0f, ringY, deckZ + halfSlotDepth + 0.5f * ring)), BoxUv::PerFace, 1.0f, onTop);
    for (const float side : {-1.0f, 1.0f}) {
        body.box(glm::vec3(ring, ringHeight, slotDepth),
                 translation(glm::vec3(side * (halfSlotWidth + 0.5f * ring), ringY, deckZ)), BoxUv::PerFace, 1.0f,
                 onTop & ~FaceMask::PosZ & ~FaceMask::NegZ);
    }
    dark.plane(glm::vec2(slotWidth, slotDepth), translation(glm::vec3(0.0f, deckTop + detail::kDecalOffset, deckZ)));

    // Free band on the base top between the deck and the chamfered front edge.
    const float bandStart = deckZ + 0.5f * deckDepth + 0.004f;
    const float bandEnd = baseFront - baseChamfer - 0.002f;

    // Power and reset buttons on the left of the band.
    const float buttonHeight = std::min(0.006f, 0.5f * (p.height - baseHeight));
    const glm::vec3 button(0.026f * k, buttonHeight, std::min(0.016f * k, 0.6f * (bandEnd - bandStart)));
    const float buttonZ = 0.5f * (bandStart + bandEnd);
    for (const float x : {-halfWidth + 0.045f * k, -halfWidth + 0.085f * k}) {
        accent.chamferedBox(button, 0.0015f * k, translation(glm::vec3(x, baseHeight + 0.5f * button.y, buttonZ)));
    }

    // Vent slats on the right of the band.
    const float ventStart = bandStart + 0.004f;
    const float ventEnd = bandEnd - 0.004f;
    const int vents = std::min(kMaxVents, static_cast<int>(std::max(0.0f, ventEnd - ventStart) / kVentPitch));
    const float ventLength = 0.25f * p.width;
    for (int i = 0; i < vents; ++i) {
        dark.box(glm::vec3(ventLength, 0.0012f, 0.003f),
                 translation(glm::vec3(halfWidth - baseChamfer - 0.012f - 0.5f * ventLength, baseHeight + 0.0006f,
                                       ventStart + (static_cast<float>(i) + 0.5f) * kVentPitch)),
                 BoxUv::PerFace, 1.0f, onTop);
    }

    // Two controller port surrounds on the front face.
    const float portHeight = std::min(0.016f * k, 0.5f * (baseHeight - 2.0f * baseChamfer));
    const float portWidth = std::min(0.03f * k, 0.15f * p.width);
    for (const float side : {-1.0f, 1.0f}) {
        dark.box(glm::vec3(portWidth, portHeight, kPortDepth),
                 translation(glm::vec3(side * (0.5f * portWidth + 0.012f * k), 0.45f * baseHeight,
                                       baseFront + 0.5f * kPortDepth)),
                 BoxUv::PerFace, 1.0f, FaceMask::All & ~FaceMask::NegZ);
    }

    out.addSocket("CartridgeSlot", SocketKind::Anchor, translation(glm::vec3(0.0f, p.height, deckZ)),
                  core::Aabb::fromMinMax(glm::vec3(-halfSlotWidth, 0.0f, -halfSlotDepth),
                                         glm::vec3(halfSlotWidth, 0.0f, halfSlotDepth)));
    out.addSocket("Front", SocketKind::Other, translation(glm::vec3(0.0f, 0.5f * baseHeight, baseFront)),
                  core::Aabb::fromMinMax(glm::vec3(-halfWidth, -0.5f * baseHeight, 0.0f),
                                         glm::vec3(halfWidth, 0.5f * baseHeight, 0.0f)));
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildConsoleCartridge(const ConsoleCartridgeParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
