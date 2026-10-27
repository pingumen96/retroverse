#pragma once

// Private to rv_cli_core: the world's snapshots as text and JSON (ADR-0015 §6, I4b step 1), shared by worldplan
// snapshots and worldplan undo.

#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rv/world/world_snapshots.hpp>

#include "catalog_text.hpp"

namespace rv::cli {

/// A digest shortened for a line of text ("1a2b3c4d5e6f"), "none" for a file the snapshot did not hold.
inline std::string shortDigest(std::string_view digest) {
    return digest.empty() ? std::string("none") : std::string(digest.substr(0, 12));
}

/// "regenerate of world", "undo of room:snes".
inline std::string snapshotActionText(const world::WorldSnapshot& snapshot) {
    return snapshot.scope.empty() ? snapshot.action : fmt::format("{} of {}", snapshot.action, snapshot.scope);
}

/// One snapshot as JSON: name, sequence, kind, action, scope, writer, takenAt, the digests (null for a file it did not
/// hold), undone, undoes (null unless an undo snapshot).
inline CatalogJson snapshotJson(const world::WorldSnapshot& snapshot) {
    const auto digest = [](const std::string& value) {
        return value.empty() ? CatalogJson(nullptr) : CatalogJson(value);
    };
    CatalogJson json;
    json["name"] = snapshot.name;
    json["sequence"] = snapshot.sequence;
    json["kind"] = std::string(world::toString(snapshot.kind));
    json["action"] = snapshot.action;
    json["scope"] = snapshot.scope;
    json["writer"] = snapshot.writer;
    json["takenAt"] = snapshot.takenAt;
    json["planDigest"] = digest(snapshot.planDigest);
    json["metaDigest"] = digest(snapshot.metaDigest);
    json["overridesDigest"] = digest(snapshot.overridesDigest);
    json["undone"] = snapshot.undone;
    json["undoes"] = snapshot.undoes.empty() ? CatalogJson(nullptr) : CatalogJson(snapshot.undoes);
    return json;
}

} // namespace rv::cli
