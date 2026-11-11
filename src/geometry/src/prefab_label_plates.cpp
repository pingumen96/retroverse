#include <algorithm>
#include <cmath>
#include <string>

#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::translation;

constexpr std::string_view kKind = "label_plates";
constexpr std::size_t kMaxPlates = 1024;
/// How far a frame's axes may be from unit length and from each other's normal (their dot product).
constexpr float kRigidTolerance = 1.0e-3f;
/// The face stands this far in front of the plate's front (detail::kDecalOffset's rule, a little more: the plates are
/// seen up close at a grazing angle).
constexpr float kFaceOffset = 0.0003f;
/// The face keeps at least this much of the plate's smaller edge.
constexpr float kMinFace = 0.004f;

Status plateError(std::size_t index, std::string_view what, std::string message) {
    return makeError(ErrorCode::OutOfRange, std::move(message),
                     std::string(kKind) + ".plates[" + std::to_string(index) + "]." + std::string(what));
}

bool finite(const glm::mat4& m) noexcept {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (!std::isfinite(m[c][r])) {
                return false;
            }
        }
    }
    return true;
}

bool rigid(const glm::mat4& m) noexcept {
    const glm::vec3 x(m[0]);
    const glm::vec3 y(m[1]);
    const glm::vec3 z(m[2]);
    const auto unit = [](const glm::vec3& v) {
        return std::abs(glm::length(v) - 1.0f) <= kRigidTolerance;
    };
    return unit(x) && unit(y) && unit(z) && std::abs(glm::dot(x, y)) <= kRigidTolerance &&
           std::abs(glm::dot(y, z)) <= kRigidTolerance && std::abs(glm::dot(z, x)) <= kRigidTolerance &&
           glm::dot(glm::cross(x, y), z) > 0.0f && m[0][3] == 0.0f && m[1][3] == 0.0f && m[2][3] == 0.0f &&
           m[3][3] == 1.0f;
}

Status validate(const LabelPlatesParams& p) {
    if (p.plates.empty() || p.plates.size() > kMaxPlates) {
        return makeError(ErrorCode::OutOfRange, "expected 1 to " + std::to_string(kMaxPlates) + " plates",
                         std::string(kKind) + ".plates");
    }
    if (Status ranges = firstError({
            checkRange(kKind, "thickness", p.thickness, 0.0005f, 0.02f),
            checkRange(kKind, "inset", p.inset, 0.0f, 0.5f),
        });
        !ranges) {
        return ranges;
    }
    for (std::size_t i = 0; i < p.plates.size(); ++i) {
        const LabelPlate& plate = p.plates[i];
        if (!finite(plate.frame) || !rigid(plate.frame)) {
            return plateError(i, "frame", "expected a rigid transform");
        }
        if (!std::isfinite(plate.size.x) || !std::isfinite(plate.size.y) || plate.size.x < 0.005f ||
            plate.size.y < 0.005f || plate.size.x > 1.0f || plate.size.y > 1.0f) {
            return plateError(i, "size", "expected sizes in [0.005, 1]");
        }
        const glm::vec4& cell = plate.cell;
        if (!std::isfinite(cell.x) || !std::isfinite(cell.y) || !std::isfinite(cell.z) || !std::isfinite(cell.w) ||
            cell.x < 0.0f || cell.y < 0.0f || cell.z > 1.0f || cell.w > 1.0f || cell.x >= cell.z || cell.y >= cell.w) {
            return plateError(i, "cell", "expected a cell inside [0, 1] with u0 < u1 and v0 < v1");
        }
        if (2.0f * p.inset > std::min(plate.size.x, plate.size.y) - kMinFace) {
            return makeError(ErrorCode::OutOfRange, "the inset leaves plate " + std::to_string(i) + " no face",
                             std::string(kKind) + ".inset");
        }
    }
    return {};
}

ProceduralPrefab assemble(const LabelPlatesParams& p) {
    detail::PrefabAssembler out(kKind, {"plate", "picture"});
    MeshBuilder& plate = out.part("plate");
    MeshBuilder& picture = out.part("picture");
    for (const LabelPlate& label : p.plates) {
        plate.box(glm::vec3(label.size, p.thickness),
                  label.frame * translation(glm::vec3(0.0f, 0.0f, 0.5f * p.thickness)), BoxUv::WorldMeters, 0.1f);
        picture.quad(label.size - glm::vec2(2.0f * p.inset),
                     label.frame * translation(glm::vec3(0.0f, 0.0f, p.thickness + kFaceOffset)), label.cell);
    }
    out.setColliders({});
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildLabelPlates(const LabelPlatesParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
