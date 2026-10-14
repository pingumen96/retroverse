#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <rv/geometry/prefabs.hpp>

#include "mesh_detail.hpp"
#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::formatNumber;
using detail::outOfRange;
using detail::translation;

constexpr std::string_view kKind = "crt";
/// Distance of the ScreenLook anchor in front of the bezel.
constexpr float kLookDistance = 0.9f;
/// The control strip under the bezel sits this far behind the bezel front.
constexpr float kStripRecess = 0.006f;
constexpr int kScreenColumns = 16;
constexpr int kScreenRows = 12;
constexpr int kVentSlats = 6;

/// Axis-aligned rectangle in the XY plane.
struct Rect {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;

    [[nodiscard]] Rect inset(float amount) const noexcept {
        return {x0 + amount, y0 + amount, x1 - amount, y1 - amount};
    }
    [[nodiscard]] float width() const noexcept { return x1 - x0; }
    [[nodiscard]] float height() const noexcept { return y1 - y0; }
};

struct CrtLayout {
    float stripHeight = 0.0f; ///< dark control strip under the bezel
    float bezelDepth = 0.0f;  ///< thickness of the bezel slab in front of the cabinet
    float frontDepth = 0.0f;  ///< bezel + front cabinet section, the rest is the tapered rear housing
    float zFront = 0.0f;      ///< bezel front plane
    float zScreen = 0.0f;     ///< screen rim plane
    float bulge = 0.0f;       ///< screen center bulge towards +Z
    float slope = 0.0f;       ///< how far the bezel inner walls widen from the screen rim to the bezel front
    float cabinetChamfer = 0.0f;
    float bezelChamfer = 0.0f;
    Rect screen;
};

[[nodiscard]] CrtLayout layoutOf(const CrtParams& p) {
    CrtLayout l;
    l.stripHeight = std::clamp(0.1f * p.height, 0.03f, 0.08f);
    l.bezelDepth = std::max(0.02f, p.screenInset + 0.008f);
    l.frontDepth = 0.38f * p.depth;
    l.zFront = 0.5f * p.depth;
    l.zScreen = l.zFront - p.screenInset;
    l.bulge = 0.5f * p.screenInset;
    l.slope = std::min(p.screenInset, 0.4f * p.bezel);
    l.cabinetChamfer = std::min(0.015f, 0.04f * std::min(p.width, p.height));
    l.bezelChamfer = std::min({0.008f, 0.4f * l.bezelDepth, 0.3f * p.bezel});
    // Largest screenAspect rectangle inside the bezel, centered in the available area.
    const float availableWidth = p.width - 2.0f * p.bezel;
    const float availableHeight = p.height - l.stripHeight - 2.0f * p.bezel;
    float screenWidth = availableWidth;
    float screenHeight = availableWidth / p.screenAspect;
    if (screenHeight > availableHeight) {
        screenHeight = availableHeight;
        screenWidth = availableHeight * p.screenAspect;
    }
    const float centerY = l.stripHeight + p.bezel + 0.5f * availableHeight;
    l.screen = {-0.5f * screenWidth, centerY - 0.5f * screenHeight, 0.5f * screenWidth, centerY + 0.5f * screenHeight};
    return l;
}

Status validate(const CrtParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "width", p.width, 0.2f, 2.0f),
            checkRange(kKind, "height", p.height, 0.2f, 2.0f),
            checkRange(kKind, "depth", p.depth, 0.15f, 2.0f),
            checkRange(kKind, "screenAspect", p.screenAspect, 0.5f, 3.0f),
            checkRange(kKind, "bezel", p.bezel, 0.01f, 0.25f * std::min(p.width, p.height)),
            checkRange(kKind, "screenInset", p.screenInset, 0.0f, 0.05f),
            checkRange(kKind, "backTaper", p.backTaper, 0.0f, 0.8f),
        });
        !status) {
        return status;
    }
    const CrtLayout l = layoutOf(p);
    if (l.bezelDepth + 0.02f > l.frontDepth) {
        return outOfRange(kKind, "screenInset",
                          "a " + formatNumber(p.screenInset) + " m inset does not fit the " +
                              formatNumber(l.frontDepth) + " m front cabinet of a " + formatNumber(p.depth) +
                              " m deep set");
    }
    return ok();
}

void addQuad(MeshData& mesh, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d,
             const glm::vec3& outward) {
    const std::array<glm::vec3, 4> points{a, b, c, d};
    detail::addPlanarPolygon(mesh, points, outward);
}

/// Front frame: flat front ring with a chamfered outer edge, straight outer sides, a back face against the
/// cabinet, and inner walls sloping from the opening down to the recessed screen rim.
[[nodiscard]] MeshData bezelMesh(const CrtParams& p, const CrtLayout& l) {
    MeshData mesh;
    const float zf = l.zFront;
    const float zc = zf - l.bezelChamfer;
    const float zb = zf - l.bezelDepth;
    const float zs = l.zScreen;
    const Rect outer{-0.5f * p.width, l.stripHeight, 0.5f * p.width, p.height};
    const Rect face = outer.inset(l.bezelChamfer);
    const Rect opening = l.screen.inset(-l.slope);
    const Rect& s = l.screen;
    const auto at = [](float x, float y, float z) {
        return glm::vec3(x, y, z);
    };
    const glm::vec3 front(0.0f, 0.0f, 1.0f);

    // Front ring (mitered trapezoids between the chamfer and the opening).
    addQuad(mesh, at(face.x0, face.y1, zf), at(face.x1, face.y1, zf), at(opening.x1, opening.y1, zf),
            at(opening.x0, opening.y1, zf), front);
    addQuad(mesh, at(opening.x0, opening.y0, zf), at(opening.x1, opening.y0, zf), at(face.x1, face.y0, zf),
            at(face.x0, face.y0, zf), front);
    addQuad(mesh, at(face.x0, face.y0, zf), at(opening.x0, opening.y0, zf), at(opening.x0, opening.y1, zf),
            at(face.x0, face.y1, zf), front);
    addQuad(mesh, at(opening.x1, opening.y0, zf), at(face.x1, face.y0, zf), at(face.x1, face.y1, zf),
            at(opening.x1, opening.y1, zf), front);
    // Chamfer around the front edge.
    addQuad(mesh, at(face.x0, face.y1, zf), at(face.x1, face.y1, zf), at(outer.x1, outer.y1, zc),
            at(outer.x0, outer.y1, zc), glm::vec3(0.0f, 1.0f, 1.0f));
    addQuad(mesh, at(face.x0, face.y0, zf), at(face.x1, face.y0, zf), at(outer.x1, outer.y0, zc),
            at(outer.x0, outer.y0, zc), glm::vec3(0.0f, -1.0f, 1.0f));
    addQuad(mesh, at(face.x0, face.y0, zf), at(face.x0, face.y1, zf), at(outer.x0, outer.y1, zc),
            at(outer.x0, outer.y0, zc), glm::vec3(-1.0f, 0.0f, 1.0f));
    addQuad(mesh, at(face.x1, face.y0, zf), at(face.x1, face.y1, zf), at(outer.x1, outer.y1, zc),
            at(outer.x1, outer.y0, zc), glm::vec3(1.0f, 0.0f, 1.0f));
    // Outer sides and back.
    addQuad(mesh, at(outer.x0, outer.y1, zc), at(outer.x1, outer.y1, zc), at(outer.x1, outer.y1, zb),
            at(outer.x0, outer.y1, zb), glm::vec3(0.0f, 1.0f, 0.0f));
    addQuad(mesh, at(outer.x0, outer.y0, zc), at(outer.x1, outer.y0, zc), at(outer.x1, outer.y0, zb),
            at(outer.x0, outer.y0, zb), glm::vec3(0.0f, -1.0f, 0.0f));
    addQuad(mesh, at(outer.x0, outer.y0, zc), at(outer.x0, outer.y1, zc), at(outer.x0, outer.y1, zb),
            at(outer.x0, outer.y0, zb), glm::vec3(-1.0f, 0.0f, 0.0f));
    addQuad(mesh, at(outer.x1, outer.y0, zc), at(outer.x1, outer.y1, zc), at(outer.x1, outer.y1, zb),
            at(outer.x1, outer.y0, zb), glm::vec3(1.0f, 0.0f, 0.0f));
    addQuad(mesh, at(outer.x0, outer.y0, zb), at(outer.x1, outer.y0, zb), at(outer.x1, outer.y1, zb),
            at(outer.x0, outer.y1, zb), glm::vec3(0.0f, 0.0f, -1.0f));
    // Inner walls down to the screen rim.
    if (zf - zs > 1e-5f && l.slope > 1e-5f) {
        addQuad(mesh, at(opening.x0, opening.y1, zf), at(opening.x1, opening.y1, zf), at(s.x1, s.y1, zs),
                at(s.x0, s.y1, zs), glm::vec3(0.0f, -1.0f, 1.0f));
        addQuad(mesh, at(opening.x0, opening.y0, zf), at(opening.x1, opening.y0, zf), at(s.x1, s.y0, zs),
                at(s.x0, s.y0, zs), glm::vec3(0.0f, 1.0f, 1.0f));
        addQuad(mesh, at(opening.x0, opening.y0, zf), at(opening.x0, opening.y1, zf), at(s.x0, s.y1, zs),
                at(s.x0, s.y0, zs), glm::vec3(1.0f, 0.0f, 1.0f));
        addQuad(mesh, at(opening.x1, opening.y0, zf), at(opening.x1, opening.y1, zf), at(s.x1, s.y1, zs),
                at(s.x1, s.y0, zs), glm::vec3(-1.0f, 0.0f, 1.0f));
    }
    return mesh;
}

/// Screen: a grid over the screen rectangle bulging by up to `bulge` at the center (zero at the rim), smooth
/// normals, uv 0..1 with (0, 0) at the top-left corner.
[[nodiscard]] MeshData screenMesh(const CrtLayout& l) {
    MeshData mesh;
    const float halfWidth = 0.5f * l.screen.width();
    const float halfHeight = 0.5f * l.screen.height();
    const glm::vec2 center(0.5f * (l.screen.x0 + l.screen.x1), 0.5f * (l.screen.y0 + l.screen.y1));
    for (int row = 0; row <= kScreenRows; ++row) {
        const float v = static_cast<float>(row) / static_cast<float>(kScreenRows);
        const float y = halfHeight * (1.0f - 2.0f * v);
        const float fy = 1.0f - (y / halfHeight) * (y / halfHeight);
        for (int column = 0; column <= kScreenColumns; ++column) {
            const float u = static_cast<float>(column) / static_cast<float>(kScreenColumns);
            const float x = halfWidth * (2.0f * u - 1.0f);
            const float fx = 1.0f - (x / halfWidth) * (x / halfWidth);
            const float dzdx = l.bulge * fy * (-2.0f * x / (halfWidth * halfWidth));
            const float dzdy = l.bulge * fx * (-2.0f * y / (halfHeight * halfHeight));
            const glm::vec3 normal = detail::normalizeOr(glm::vec3(-dzdx, -dzdy, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            const glm::vec3 tangent =
                detail::orthonormalize(glm::vec3(1.0f, 0.0f, dzdx), normal, glm::vec3(1.0f, 0.0f, 0.0f));
            detail::addVertex(mesh, glm::vec3(center.x + x, center.y + y, l.zScreen + l.bulge * fx * fy), normal,
                              glm::vec4(tangent, -1.0f), glm::vec2(u, v));
        }
    }
    const auto columns = static_cast<std::uint32_t>(kScreenColumns + 1);
    for (std::uint32_t row = 0; row < static_cast<std::uint32_t>(kScreenRows); ++row) {
        for (std::uint32_t column = 0; column < static_cast<std::uint32_t>(kScreenColumns); ++column) {
            const std::uint32_t topLeft = row * columns + column;
            const std::uint32_t bottomLeft = topLeft + columns;
            detail::addTriangle(mesh, topLeft, bottomLeft, bottomLeft + 1);
            detail::addTriangle(mesh, topLeft, bottomLeft + 1, topLeft + 1);
        }
    }
    return mesh;
}

using Octagon = std::array<glm::vec3, 8>;

/// Octagon (rectangle with cut corners) listed counter-clockwise from the bottom-left cut.
[[nodiscard]] Octagon octagon(const Rect& r, float cut, float z) {
    return {glm::vec3(r.x0 + cut, r.y0, z), glm::vec3(r.x1 - cut, r.y0, z), glm::vec3(r.x1, r.y0 + cut, z),
            glm::vec3(r.x1, r.y1 - cut, z), glm::vec3(r.x1 - cut, r.y1, z), glm::vec3(r.x0 + cut, r.y1, z),
            glm::vec3(r.x0, r.y1 - cut, z), glm::vec3(r.x0, r.y0 + cut, z)};
}

struct Housing {
    MeshData mesh;
    /// Sections from the cabinet back to the rear face: start, shoulder, rear.
    std::array<Octagon, 3> sections;
};

/// Rear housing: an octagonal loft from the back of the front cabinet to the rear face with a convex profile
/// (a gentle first stage, then a steeper one), open at the front where the cabinet's back face closes it. The
/// rear face is centered on the screen height when it fits.
[[nodiscard]] Housing housingMesh(const CrtParams& p, const CrtLayout& l) {
    // Fraction of the housing depth and of the taper covered by the gentle first stage.
    constexpr float kShoulderDepth = 0.45f;
    constexpr float kShoulderTaper = 0.2f;
    constexpr float kCornerCut = 0.14f;

    Housing housing;
    const float zStart = l.zFront - l.frontDepth;
    const float zEnd = -0.5f * p.depth;
    const Rect start{-0.5f * p.width + l.cabinetChamfer, l.cabinetChamfer, 0.5f * p.width - l.cabinetChamfer,
                     p.height - l.cabinetChamfer};
    const float startY = 0.5f * (start.y0 + start.y1);
    const float screenY = 0.5f * (l.screen.y0 + l.screen.y1);
    const float cut = kCornerCut * std::min(start.width(), start.height());
    // Section scaled by `scale` around a center that moves from the cabinet center towards the screen height.
    const auto section = [&](float scale, float towardsScreen, float z) {
        const float halfWidth = 0.5f * scale * start.width();
        const float halfHeight = 0.5f * scale * start.height();
        const float y =
            std::clamp(startY + towardsScreen * (screenY - startY), start.y0 + halfHeight, start.y1 - halfHeight);
        return octagon(Rect{-halfWidth, y - halfHeight, halfWidth, y + halfHeight}, scale * cut, z);
    };
    housing.sections[0] = octagon(start, cut, zStart);
    housing.sections[1] =
        section(1.0f - kShoulderTaper * p.backTaper, kShoulderDepth, zStart + kShoulderDepth * (zEnd - zStart));
    housing.sections[2] = section(1.0f - p.backTaper, 1.0f, zEnd);

    glm::vec3 interior(0.0f);
    for (const Octagon& ring : housing.sections) {
        for (const glm::vec3& point : ring) {
            interior += point;
        }
    }
    interior /= 24.0f;
    for (std::size_t stage = 0; stage + 1 < housing.sections.size(); ++stage) {
        const Octagon& nearRing = housing.sections[stage];
        const Octagon& farRing = housing.sections[stage + 1];
        for (std::size_t i = 0; i < nearRing.size(); ++i) {
            const std::size_t next = (i + 1) % nearRing.size();
            const glm::vec3 center = 0.25f * (nearRing[i] + nearRing[next] + farRing[next] + farRing[i]);
            addQuad(housing.mesh, nearRing[i], nearRing[next], farRing[next], farRing[i], center - interior);
        }
    }
    detail::addPlanarPolygon(housing.mesh, housing.sections[2], glm::vec3(0.0f, 0.0f, -1.0f));
    return housing;
}

ProceduralPrefab assemble(const CrtParams& p) {
    const CrtLayout l = layoutOf(p);
    detail::PrefabAssembler out(kKind, {"plastic_body", "plastic_bezel", "screen", "plastic_dark"});
    MeshBuilder& body = out.part("plastic_body");
    MeshBuilder& dark = out.part("plastic_dark");

    // Front cabinet section behind the bezel, then the tapered rear housing.
    const float cabinetDepth = l.frontDepth - l.bezelDepth;
    body.chamferedBox(glm::vec3(p.width, p.height, cabinetDepth), l.cabinetChamfer,
                      translation(glm::vec3(0.0f, 0.5f * p.height, l.zFront - l.bezelDepth - 0.5f * cabinetDepth)));
    const Housing housing = housingMesh(p, l);
    body.append(housing.mesh);

    out.part("plastic_bezel").append(bezelMesh(p, l));
    out.part("screen").append(screenMesh(l));

    // Control strip under the bezel, with a row of small buttons and the power button on the right.
    const float stripFront = l.zFront - kStripRecess;
    dark.box(glm::vec3(p.width, l.stripHeight, stripFront - (l.zFront - l.bezelDepth)),
             translation(glm::vec3(0.0f, 0.5f * l.stripHeight, 0.5f * (stripFront + l.zFront - l.bezelDepth))));
    const float k = std::clamp(std::min(p.width / 0.71f, l.stripHeight / 0.061f), 0.3f, 1.5f);
    const float buttonY = 0.5f * l.stripHeight;
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 size(0.012f * k, 0.007f * k, 0.005f);
        const float x = 0.5f * p.width - (0.11f + 0.024f * static_cast<float>(i)) * k;
        dark.chamferedBox(size, 0.0012f * k, translation(glm::vec3(x, buttonY, stripFront)));
    }
    const glm::vec3 power(0.022f * k, 0.012f * k, 0.006f);
    const float powerX = 0.5f * p.width - 0.06f * k;
    dark.chamferedBox(power, 0.0015f * k, translation(glm::vec3(powerX, buttonY, stripFront)));
    const float powerFront = stripFront + 0.5f * power.z;
    out.addSocket("PowerButton", SocketKind::Other, translation(glm::vec3(powerX, buttonY, powerFront)),
                  core::Aabb::fromMinMax(glm::vec3(-0.5f * power.x, -0.5f * power.y, 0.0f),
                                         glm::vec3(0.5f * power.x, 0.5f * power.y, 0.0f)));

    // Vent slats across the steeper top face of the rear housing (octagon edge 4-5 is the top).
    const Octagon& shoulder = housing.sections[1];
    const Octagon& rear = housing.sections[2];
    const glm::vec3 frontMid = 0.5f * (shoulder[4] + shoulder[5]);
    const glm::vec3 backMid = 0.5f * (rear[4] + rear[5]);
    const float frontWidth = shoulder[4].x - shoulder[5].x;
    const float backWidth = rear[4].x - rear[5].x;
    const glm::vec3 alongX(1.0f, 0.0f, 0.0f);
    const glm::vec3 topNormal =
        detail::normalizeOr(glm::cross(alongX, detail::normalizeOr(backMid - frontMid, glm::vec3(0.0f, 0.0f, -1.0f))),
                            glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 upSlope = glm::cross(alongX, topNormal);
    constexpr float kSlatHeight = 0.002f;
    constexpr float kFirstSlat = 0.3f; ///< slats span 30% to 75% of the face, front to back
    constexpr float kSlatSpan = 0.45f;
    // Slats never touch each other, even on a very shallow set.
    const float pitch = kSlatSpan / static_cast<float>(kVentSlats - 1) * glm::length(backMid - frontMid);
    const float slatDepth = std::min(0.008f, 0.6f * pitch);
    for (int i = 0; i < kVentSlats && slatDepth > 0.001f; ++i) {
        const float t = kFirstSlat + kSlatSpan * static_cast<float>(i) / static_cast<float>(kVentSlats - 1);
        const glm::vec3 onFace = frontMid + t * (backMid - frontMid);
        glm::mat4 frame(1.0f);
        frame[0] = glm::vec4(alongX, 0.0f);
        frame[1] = glm::vec4(topNormal, 0.0f);
        frame[2] = glm::vec4(upSlope, 0.0f);
        frame[3] = glm::vec4(onFace + 0.5f * kSlatHeight * topNormal, 1.0f);
        const float length = 0.55f * (frontWidth + t * (backWidth - frontWidth));
        dark.box(glm::vec3(length, kSlatHeight, slatDepth), frame, BoxUv::PerFace, 1.0f,
                 FaceMask::All & ~FaceMask::NegY);
    }

    const glm::vec2 screenCenter(0.5f * (l.screen.x0 + l.screen.x1), 0.5f * (l.screen.y0 + l.screen.y1));
    out.addSocket("ScreenSurface", SocketKind::ScreenSurface, translation(glm::vec3(screenCenter, l.zScreen)),
                  core::Aabb::fromMinMax(glm::vec3(-0.5f * l.screen.width(), -0.5f * l.screen.height(), 0.0f),
                                         glm::vec3(0.5f * l.screen.width(), 0.5f * l.screen.height(), l.bulge)));
    out.addSocket("TV_Stand", SocketKind::Anchor, glm::mat4(1.0f));
    out.addSocket("ScreenLook", SocketKind::Anchor, translation(glm::vec3(screenCenter, l.zFront + kLookDistance)));
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildCrt(const CrtParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
