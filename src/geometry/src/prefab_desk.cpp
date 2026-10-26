// I6 step 3b's study desk (the computer studio's hero corner, ADR-0014 §1 amended): a desk with a drawer pedestal and,
// on it, a generic home computer of the 80s and 90s (original design: a flat system unit, a monitor, a keyboard).

#include <algorithm>
#include <string>

#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::formatNumber;
using detail::outOfRange;
using detail::translation;

constexpr std::string_view kKind = "desk";
constexpr float kTop = 0.03f;
constexpr float kPanel = 0.022f;
/// The pedestal's and the side panel's inset from the top's edges.
constexpr float kInset = 0.02f;
constexpr int kDrawers = 3;
constexpr float kReveal = 0.004f;
constexpr float kFront = 0.018f;
/// The computer: system unit, monitor shell (and its tapered back), screen, keyboard (meters).
constexpr glm::vec3 kUnit{0.42f, 0.1f, 0.34f};
constexpr glm::vec3 kShell{0.38f, 0.33f, 0.24f};
constexpr glm::vec3 kShellBack{0.28f, 0.25f, 0.08f};
constexpr glm::vec2 kScreen{0.27f, 0.2f};
constexpr glm::vec3 kKeyboard{0.44f, 0.03f, 0.15f};
/// The smallest depth that holds the computer and the keyboard in front of it.
constexpr float kComputerMinDepth = 0.6f;
constexpr BoxUv kGrain = BoxUv::GrainMeters;
constexpr float kWoodTile = detail::kWoodMetersPerTile;

Status validate(const DeskParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "width", p.width, 0.8f, 2.4f),
            checkRange(kKind, "depth", p.depth, 0.5f, 1.0f),
            checkRange(kKind, "height", p.height, 0.6f, 0.9f),
        });
        !status) {
        return status;
    }
    if (p.computer && p.depth < kComputerMinDepth) {
        return outOfRange(kKind, "depth",
                          "a desk with the computer must be at least " + formatNumber(kComputerMinDepth) + " m deep");
    }
    return ok();
}

ProceduralPrefab assemble(const DeskParams& p) {
    detail::PrefabAssembler out(kKind,
                                {"wood_primary", "wood_secondary", "metal_trim", "plastic_body", "plastic_dark", "screen"});
    MeshBuilder& wood = out.part("wood_primary");
    MeshBuilder& fronts = out.part("wood_secondary");
    MeshBuilder& metal = out.part("metal_trim");

    const float halfWidth = 0.5f * p.width;
    const float halfDepth = 0.5f * p.depth;
    const float underTop = p.height - kTop;
    // The top, its upper edges bevelled.
    wood.chamferedBox(glm::vec3(p.width, kTop, p.depth), 0.004f,
                      translation(glm::vec3(0.0f, p.height - 0.5f * kTop, 0.0f)), kGrain, kWoodTile, FaceMask::PosY);
    // The drawer pedestal on the left, the side panel on the right, the modesty panel between them at the back.
    const float carcassDepth = p.depth - 2.0f * kInset;
    const float pedestalWidth = std::min(0.42f, 0.34f * p.width);
    const float pedestalX = -halfWidth + kInset + 0.5f * pedestalWidth;
    const float carcassFront = halfDepth - kInset - kFront;
    wood.chamferedBox(glm::vec3(pedestalWidth, underTop, carcassDepth - kFront), 0.002f,
                      translation(glm::vec3(pedestalX, 0.5f * underTop, -0.5f * kFront)), kGrain, kWoodTile);
    const float sideX = halfWidth - kInset - 0.5f * kPanel;
    wood.chamferedBox(glm::vec3(kPanel, underTop, carcassDepth), 0.002f,
                      translation(glm::vec3(sideX, 0.5f * underTop, 0.0f)), kGrain, kWoodTile);
    const float modestyLeft = pedestalX + 0.5f * pedestalWidth;
    const float modestyRight = sideX - 0.5f * kPanel;
    const float modestyHeight = std::min(0.4f, underTop - 0.1f);
    wood.box(glm::vec3(modestyRight - modestyLeft, modestyHeight, kPanel),
             translation(glm::vec3(0.5f * (modestyLeft + modestyRight), underTop - 0.5f * modestyHeight,
                                   -halfDepth + kInset + 0.5f * kPanel)),
             kGrain, kWoodTile);
    // Three drawers with bar handles on the pedestal's front, on a recessed plinth.
    const float plinth = 0.06f;
    const float drawer = (underTop - plinth - static_cast<float>(kDrawers + 1) * kReveal) / static_cast<float>(kDrawers);
    for (int i = 0; i < kDrawers; ++i) {
        const float top = underTop - kReveal - static_cast<float>(i) * (drawer + kReveal);
        fronts.chamferedBox(glm::vec3(pedestalWidth - 2.0f * kReveal, drawer, kFront), 0.003f,
                            translation(glm::vec3(pedestalX, top - 0.5f * drawer, carcassFront + 0.5f * kFront)), kGrain,
                            kWoodTile);
        metal.box(glm::vec3(std::min(0.14f, 0.5f * pedestalWidth), 0.012f, 0.012f),
                  translation(glm::vec3(pedestalX, top - 0.3f * drawer, halfDepth - kInset + 0.006f)));
    }

    out.addSocket("Top", SocketKind::Anchor, translation(glm::vec3(0.0f, p.height, 0.0f)));
    if (!p.computer) {
        return std::move(out).finish();
    }

    // The computer, a little right of the middle: the system unit at the back, the monitor on it, the keyboard in
    // front of it.
    MeshBuilder& body = out.part("plastic_body");
    MeshBuilder& dark = out.part("plastic_dark");
    const float x = std::min(0.15f, halfWidth - kInset - 0.5f * kUnit.x - 0.05f);
    const float unitBack = -halfDepth + 0.03f;
    const float unitZ = unitBack + 0.5f * kUnit.z;
    body.chamferedBox(kUnit, 0.006f, translation(glm::vec3(x, p.height + 0.5f * kUnit.y, unitZ)));
    // Its front: a drive's slot, a power light's housing, vents.
    const float unitFront = unitBack + kUnit.z;
    dark.box(glm::vec3(0.12f, 0.008f, 0.004f),
             translation(glm::vec3(x + 0.09f, p.height + 0.06f, unitFront + 0.001f)));
    for (int i = 0; i < 5; ++i) {
        dark.box(glm::vec3(0.1f, 0.004f, 0.004f),
                 translation(glm::vec3(x - 0.11f, p.height + 0.03f + 0.012f * static_cast<float>(i), unitFront + 0.001f)));
    }
    // The monitor: its tapered back and its shell, standing on the unit, the bezel and the screen on its front.
    const float monitorBottom = p.height + kUnit.y;
    const float shellBack = unitBack + 0.02f + kShellBack.z;
    body.chamferedBox(kShellBack, 0.01f,
                      translation(glm::vec3(x, monitorBottom + 0.5f * kShellBack.y, shellBack - 0.5f * kShellBack.z)));
    body.chamferedBox(kShell, 0.012f,
                      translation(glm::vec3(x, monitorBottom + 0.5f * kShell.y, shellBack + 0.5f * kShell.z)));
    const float shellFront = shellBack + kShell.z;
    const float screenY = monitorBottom + 0.56f * kShell.y;
    dark.box(glm::vec3(kScreen.x + 0.03f, kScreen.y + 0.03f, 0.006f),
             translation(glm::vec3(x, screenY, shellFront + 0.003f)));
    const float screenZ = shellFront + 0.006f + detail::kDecalOffset;
    out.part("screen").quad(kScreen, translation(glm::vec3(x, screenY, screenZ)));
    out.addSocket("Screen", SocketKind::Other, translation(glm::vec3(x, screenY, screenZ)),
                  core::Aabb::fromMinMax(glm::vec3(-0.5f * kScreen.x, -0.5f * kScreen.y, 0.0f),
                                         glm::vec3(0.5f * kScreen.x, 0.5f * kScreen.y, 0.0f)));
    // The keyboard in front of the monitor, its keys in four rows.
    const float keyboardZ = std::min(shellFront + 0.05f + 0.5f * kKeyboard.z, halfDepth - 0.04f - 0.5f * kKeyboard.z);
    body.chamferedBox(kKeyboard, 0.006f, translation(glm::vec3(x, p.height + 0.5f * kKeyboard.y, keyboardZ)));
    const float keyTop = p.height + kKeyboard.y;
    constexpr int kKeysPerRow = 13;
    const float keyPitch = (kKeyboard.x - 0.04f) / static_cast<float>(kKeysPerRow);
    for (int row = 0; row < 4; ++row) {
        const float z = keyboardZ - 0.045f + 0.028f * static_cast<float>(row);
        for (int key = 0; key < kKeysPerRow; ++key) {
            const float keyX = x - 0.5f * kKeyboard.x + 0.02f + (static_cast<float>(key) + 0.5f) * keyPitch;
            dark.box(glm::vec3(keyPitch - 0.004f, 0.008f, 0.022f), translation(glm::vec3(keyX, keyTop + 0.004f, z)),
                     BoxUv::PerFace, 1.0f, FaceMask::All & ~FaceMask::NegY);
        }
    }
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildDesk(const DeskParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
