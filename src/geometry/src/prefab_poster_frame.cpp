#include <algorithm>

#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::translation;

constexpr std::string_view kKind = "poster_frame";
constexpr float kBackingThickness = 0.003f;
/// Smallest picture edge left inside the frame (meters).
constexpr float kMinPicture = 0.01f;

Status validate(const PosterFrameParams& p) {
    const float maxFrame = std::min(0.5f, 0.5f * (std::min(p.width, p.height) - kMinPicture));
    return firstError({
        checkRange(kKind, "width", p.width, 0.05f, 5.0f),
        checkRange(kKind, "height", p.height, 0.05f, 5.0f),
        checkRange(kKind, "frameWidth", p.frameWidth, 0.005f, maxFrame),
        checkRange(kKind, "depth", p.depth, 0.005f, 0.2f),
    });
}

ProceduralPrefab assemble(const PosterFrameParams& p) {
    detail::PrefabAssembler out(kKind, {"frame", "picture"});
    MeshBuilder& frame = out.part("frame");

    const float halfWidth = 0.5f * p.width;
    const float halfHeight = 0.5f * p.height;
    const float bar = p.frameWidth;
    const float chamfer = std::min(0.003f, 0.2f * std::min(bar, p.depth));
    const glm::vec2 picture(p.width - 2.0f * bar, p.height - 2.0f * bar);

    // Top and bottom bars run the full width; the side bars fit between them.
    for (const float side : {-1.0f, 1.0f}) {
        frame.chamferedBox(glm::vec3(p.width, bar, p.depth), chamfer,
                           translation(glm::vec3(0.0f, side * (halfHeight - 0.5f * bar), 0.5f * p.depth)));
        frame.chamferedBox(glm::vec3(bar, picture.y, p.depth), chamfer,
                           translation(glm::vec3(side * (halfWidth - 0.5f * bar), 0.0f, 0.5f * p.depth)));
    }
    // Backing board closing the frame behind the picture (its front is hidden by the picture).
    frame.box(glm::vec3(picture, kBackingThickness), translation(glm::vec3(0.0f, 0.0f, 0.5f * kBackingThickness)),
              BoxUv::PerFace, 1.0f, FaceMask::All & ~FaceMask::PosZ);

    // Picture recessed behind the frame front, well clear of the backing board.
    const float pictureZ = std::max(kBackingThickness + 0.001f, 0.45f * p.depth);
    out.part("picture").quad(picture, translation(glm::vec3(0.0f, 0.0f, pictureZ)));
    out.addSocket("Picture", SocketKind::Other, translation(glm::vec3(0.0f, 0.0f, pictureZ)),
                  core::Aabb::fromMinMax(glm::vec3(-0.5f * picture.x, -0.5f * picture.y, 0.0f),
                                         glm::vec3(0.5f * picture.x, 0.5f * picture.y, 0.0f)));
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildPosterFrame(const PosterFrameParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
