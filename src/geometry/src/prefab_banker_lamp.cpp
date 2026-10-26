// I6 step 3b's banker's lamp (the computer studio's lamps, ADR-0014 §1 amended): a green glass half-tube shade on a
// brass stem and base, standing on a card catalogue (three columns of six small drawers, each with a brass label holder
// and pull; the step's review: a plain two-drawer stand read as a tall dark chest) so that its bulb stands about where a
// floor lamp's does.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

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

constexpr std::string_view kKind = "banker_lamp";
constexpr float kTop = 0.03f;
constexpr float kFront = 0.016f;
constexpr float kReveal = 0.005f;
constexpr float kPlinth = 0.07f;
/// The card catalogue's drawers: columns and rows; a drawer's brass label holder and its pull.
constexpr int kColumns = 3;
constexpr int kRows = 6;
constexpr glm::vec2 kLabel{0.05f, 0.024f};
constexpr float kLabelThickness = 0.003f;
constexpr glm::vec3 kPull{0.032f, 0.01f, 0.012f};
/// The carcass's front stands this far behind the top's front edge (the fronts and the knobs in front of it).
constexpr float kFrontRoom = 0.045f;
/// The lamp on the stand: its base plate, its stem, the shade's radius and wall, the shade's axis above the stand.
constexpr glm::vec3 kBasePlate{0.2f, 0.025f, 0.13f};
constexpr float kStemRadius = 0.008f;
constexpr float kShadeRadius = 0.065f;
constexpr float kShadeWall = 0.003f;
constexpr float kShadeAxis = 0.36f;
constexpr int kArcSegments = 16;
constexpr BoxUv kGrain = BoxUv::GrainMeters;
constexpr float kWoodTile = detail::kWoodMetersPerTile;

Status validate(const BankerLampParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "standWidth", p.standWidth, 0.25f, 0.8f),
            checkRange(kKind, "standDepth", p.standDepth, 0.25f, 0.6f),
            checkRange(kKind, "standHeight", p.standHeight, 0.4f, 1.4f),
            checkRange(kKind, "shadeLength", p.shadeLength, 0.15f, 0.6f),
        });
        !status) {
        return status;
    }
    if (p.shadeLength > p.standWidth) {
        return outOfRange(kKind, "shadeLength",
                          "a shade of " + formatNumber(p.shadeLength) + " m overhangs a stand " +
                              formatNumber(p.standWidth) + " m wide");
    }
    return ok();
}

/// A half tube along +X of `length`, its arc over the top (from +Z over +Y to -Z) at `radius`, centred on the origin,
/// facing outwards (`inwards`: facing the axis); uv u along the length, v around the arc.
MeshData halfTube(float radius, float length, bool inwards) {
    MeshData mesh;
    mesh.name = "half_tube";
    for (int i = 0; i <= kArcSegments; ++i) {
        const float angle = core::kPi * static_cast<float>(i) / static_cast<float>(kArcSegments);
        const glm::vec3 normal(0.0f, std::sin(angle), std::cos(angle));
        for (int end = 0; end < 2; ++end) {
            Vertex v;
            v.position = glm::vec3((static_cast<float>(end) - 0.5f) * length, 0.0f, 0.0f) + normal * radius;
            v.normal = inwards ? -normal : normal;
            v.uv0 = glm::vec2(static_cast<float>(end), static_cast<float>(i) / static_cast<float>(kArcSegments));
            mesh.vertices.push_back(v);
        }
    }
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(kArcSegments); ++i) {
        const std::uint32_t a = 2 * i;
        const std::uint32_t b = a + 1;
        const std::uint32_t c = a + 2;
        const std::uint32_t d = a + 3;
        // Outwards: counter-clockwise seen from outside.
        if (inwards) {
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        } else {
            mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
    }
    return mesh;
}

/// A half disc closing one end of the shade, in the YZ plane at x, its arc over the top, facing +X (`side` +1) or -X
/// (-1): a fan from the middle of its straight edge.
MeshData halfDisc(float radius, float x, float side) {
    MeshData mesh;
    mesh.name = "half_disc";
    Vertex centre;
    centre.position = glm::vec3(x, 0.0f, 0.0f);
    centre.normal = glm::vec3(side, 0.0f, 0.0f);
    centre.uv0 = glm::vec2(0.5f, 1.0f);
    mesh.vertices.push_back(centre);
    for (int i = 0; i <= kArcSegments; ++i) {
        const float angle = core::kPi * static_cast<float>(i) / static_cast<float>(kArcSegments);
        const glm::vec3 direction(0.0f, std::sin(angle), std::cos(angle));
        Vertex v;
        v.position = centre.position + direction * radius;
        v.normal = centre.normal;
        v.uv0 = glm::vec2(0.5f + 0.5f * direction.z, 1.0f - direction.y);
        mesh.vertices.push_back(v);
    }
    for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(kArcSegments); ++i) {
        // Counter-clockwise seen from the side it faces.
        if (side > 0.0f) {
            mesh.indices.insert(mesh.indices.end(), {0u, i + 1, i});
        } else {
            mesh.indices.insert(mesh.indices.end(), {0u, i, i + 1});
        }
    }
    return mesh;
}

ProceduralPrefab assemble(const BankerLampParams& p) {
    detail::PrefabAssembler out(kKind, {"wood_primary", "wood_secondary", "metal", "lamp_shade"});
    MeshBuilder& wood = out.part("wood_primary");
    MeshBuilder& fronts = out.part("wood_secondary");
    MeshBuilder& metal = out.part("metal");

    // The stand: a carcass under an overhanging top on a recessed plinth, a card catalogue's grid of drawers in front.
    const float halfDepth = 0.5f * p.standDepth;
    const float underTop = p.standHeight - kTop;
    const float carcassFront = halfDepth - kFrontRoom;
    const float carcassDepth = carcassFront + halfDepth - 0.01f;
    const float carcassWidth = p.standWidth - 0.02f;
    wood.chamferedBox(glm::vec3(p.standWidth, kTop, p.standDepth), 0.004f,
                      translation(glm::vec3(0.0f, p.standHeight - 0.5f * kTop, 0.0f)), kGrain, kWoodTile,
                      FaceMask::PosY);
    wood.chamferedBox(glm::vec3(carcassWidth, underTop, carcassDepth), 0.002f,
                      translation(glm::vec3(0.0f, 0.5f * underTop, carcassFront - 0.5f * carcassDepth)), kGrain,
                      kWoodTile);
    const float drawerWidth = (carcassWidth - static_cast<float>(kColumns + 1) * kReveal) / static_cast<float>(kColumns);
    const float drawerHeight = (underTop - kPlinth - static_cast<float>(kRows + 1) * kReveal) / static_cast<float>(kRows);
    const float frontZ = carcassFront + 0.5f * kFront;
    const float hardwareZ = carcassFront + kFront;
    // The label holders and pulls scale down with a small drawer.
    const float hardware = std::min(1.0f, std::min(drawerWidth / 0.11f, drawerHeight / 0.1f));
    for (int row = 0; row < kRows; ++row) {
        const float y = kPlinth + kReveal + (static_cast<float>(row) + 0.5f) * drawerHeight +
                        static_cast<float>(row) * kReveal;
        for (int column = 0; column < kColumns; ++column) {
            const float x = -0.5f * carcassWidth + kReveal + (static_cast<float>(column) + 0.5f) * drawerWidth +
                            static_cast<float>(column) * kReveal;
            fronts.chamferedBox(glm::vec3(drawerWidth, drawerHeight, kFront), 0.002f,
                                translation(glm::vec3(x, y, frontZ)), kGrain, kWoodTile);
            metal.box(glm::vec3(kLabel.x * hardware, kLabel.y * hardware, kLabelThickness),
                      translation(glm::vec3(x, y + 0.2f * drawerHeight, hardwareZ + 0.5f * kLabelThickness)));
            metal.chamferedBox(kPull * hardware, 0.003f * hardware,
                               translation(glm::vec3(x, y - 0.18f * drawerHeight,
                                                     hardwareZ + 0.5f * kPull.z * hardware)));
        }
    }

    // The lamp: a brass plate, a stem at the back, a bar under the shade, the shade over the bulb.
    const float y0 = p.standHeight;
    const float lampZ = -0.25f * halfDepth;
    metal.chamferedBox(kBasePlate, 0.008f, translation(glm::vec3(0.0f, y0 + 0.5f * kBasePlate.y, lampZ)));
    const float axisY = y0 + kShadeAxis;
    const float stemZ = lampZ - 0.5f * kBasePlate.z + 0.025f;
    metal.cylinder(kStemRadius, axisY - y0 - kBasePlate.y, 16, true,
                   translation(glm::vec3(0.0f, 0.5f * (y0 + kBasePlate.y + axisY), stemZ)));
    const float shadeZ = lampZ + 0.02f;
    metal.box(glm::vec3(0.012f, 0.012f, shadeZ - stemZ + 0.006f),
              translation(glm::vec3(0.0f, axisY + 0.006f, 0.5f * (stemZ + shadeZ))));
    metal.box(glm::vec3(p.shadeLength - 0.02f, 0.008f, 0.008f), translation(glm::vec3(0.0f, axisY + 0.004f, shadeZ)));
    MeshBuilder& shade = out.part("lamp_shade");
    const glm::mat4 shadeFrame = translation(glm::vec3(0.0f, axisY, shadeZ));
    shade.append(halfTube(kShadeRadius, p.shadeLength, false), shadeFrame)
        .append(halfTube(kShadeRadius - kShadeWall, p.shadeLength, true), shadeFrame);
    for (const float side : {-1.0f, 1.0f}) {
        // The ends, closed: a half disc at each end of the tube, seen from both sides.
        shade.append(halfDisc(kShadeRadius, side * 0.5f * p.shadeLength, side), shadeFrame)
            .append(halfDisc(kShadeRadius - kShadeWall, side * (0.5f * p.shadeLength - kShadeWall), -side), shadeFrame);
    }
    out.addSocket("LightSource", SocketKind::Anchor, translation(glm::vec3(0.0f, axisY - 0.02f, shadeZ)));
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildBankerLamp(const BankerLampParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
