// I5 step 3's secondary TV (ADR-0016 §5, style §15.1: one consumer CRT model in two sizes): a theme pack's furniture
// in a decoration socket, a low sideboard carrying a small CRT. Both are the existing models (tv_cabinet's body and
// crt's set), built with their own rules and laid together, so that the room's two TVs read as one family.

#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::formatNumber;
using detail::outOfRange;
using detail::translation;

constexpr std::string_view kKind = "secondary_tv";

Status validate(const SecondaryTvParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "width", p.width, 0.4f, 4.0f),
            checkRange(kKind, "height", p.height, 0.2f, 1.5f),
            checkRange(kKind, "depth", p.depth, 0.2f, 1.5f),
            checkRange(kKind, "tvSetBack", p.tvSetBack, 0.0f, 0.1f),
        });
        !status) {
        return status;
    }
    if (Status status =
            checkRange(kKind, "tvSize", p.tvSize, glm::vec3(0.2f, 0.2f, 0.15f),
                       glm::vec3(p.width, 1.0f, p.depth - p.tvSetBack));
        !status) {
        return status;
    }
    const float reach = 0.5f * (p.width - p.tvSize.x);
    if (!(std::abs(p.tvOffset) <= reach + 1e-6f)) {
        return outOfRange(kKind, "tvOffset",
                          formatNumber(p.tvOffset) + " m puts the CRT past the top's edge (at most " +
                              formatNumber(reach) + " m either way)");
    }
    return ok();
}

/// A sub-model's error named as this kind's parameter: the sideboard's keys are ours, the CRT's "tv<Key>" (its size
/// "tvSize").
Error renamed(Error error, std::string_view part) {
    const std::string prefix = std::string(part) + ".";
    if (!error.context.starts_with(prefix)) {
        return error;
    }
    std::string key = error.context.substr(prefix.size());
    if (part == "crt") {
        if (key == "width" || key == "height" || key == "depth") {
            key = "tvSize";
        } else if (!key.empty()) {
            key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
            key = "tv" + key;
        }
    }
    error.context = std::string(kKind) + "." + key;
    return error;
}

} // namespace

Expected<ProceduralPrefab> buildSecondaryTv(const SecondaryTvParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    TvCabinetParams sideboardParams;
    sideboardParams.width = params.width;
    sideboardParams.height = params.height;
    sideboardParams.depth = params.depth;
    sideboardParams.drawers = params.drawers;
    sideboardParams.legHeight = params.legHeight;
    auto sideboard = buildTvCabinet(sideboardParams);
    if (!sideboard) {
        return tl::unexpected(renamed(sideboard.error(), "tv_cabinet"));
    }
    CrtParams crtParams;
    crtParams.width = params.tvSize.x;
    crtParams.height = params.tvSize.y;
    crtParams.depth = params.tvSize.z;
    crtParams.bezel = params.tvBezel;
    auto crt = buildCrt(crtParams);
    if (!crt) {
        return tl::unexpected(renamed(crt.error(), "crt"));
    }

    // The CRT stands on the top, its front tvSetBack behind the top's front edge (the CRT's pivot is its floor's
    // centre, its front at +depth / 2).
    const glm::mat4 onTop = translation(
        glm::vec3(params.tvOffset, params.height, 0.5f * params.depth - params.tvSetBack - 0.5f * params.tvSize.z));
    detail::PrefabAssembler out(kKind, {"wood_primary", "wood_secondary", "metal_trim", "plastic_body",
                                        "plastic_bezel", "screen", "plastic_dark"});
    for (const PrefabPart& part : sideboard->parts) {
        out.part(part.materialRole).append(part.mesh);
    }
    for (const PrefabPart& part : crt->parts) {
        out.part(part.materialRole).append(part.mesh, onTop);
    }
    for (const Socket& socket : crt->sockets) {
        std::string_view meshRole;
        if (socket.meshIndex && *socket.meshIndex < crt->parts.size()) {
            meshRole = crt->parts[*socket.meshIndex].materialRole;
        }
        out.addSocket(socket.id, socket.kind, onTop * socket.transform, socket.bounds, meshRole);
    }
    for (const Socket& socket : sideboard->sockets) {
        if (socket.id == "ConsoleShelf") {
            out.addSocket(socket.id, socket.kind, socket.transform, socket.bounds);
        }
    }
    return std::move(out).finish();
}

} // namespace rv::geometry
