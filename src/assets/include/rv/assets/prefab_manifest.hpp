#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/assets/model_data.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/socket.hpp>

namespace rv::assets {

/// Provenance and license of the asset (style §11, §11.1).
struct SourceInfo {
    std::string kind; ///< "original", "user", "third_party", ...
    std::string url;  ///< empty when not applicable (JSON null)
    std::string author;
    std::string acquiredAt;      ///< ISO date "YYYY-MM-DD"
    std::string license;         ///< SPDX identifier or "proprietary-original"
    std::string licenseSnapshot; ///< path of the license text in assets/registry/licenses (JSON null = none)
    bool attributionRequired = false;
    bool redistributionApproved = false;
};

/// The screen that shows video or images (style §15).
struct VideoSurfaceInfo {
    std::string aspectRatio; ///< "4:3", "16:9", ...
    std::string meshNode;    ///< socket id or node name of the ScreenSurface
};

/// How the prefab is produced at runtime. Exactly one of `model` and `generator` is set (validated, not parsed).
struct RuntimeInfo {
    std::optional<std::string> model;                 ///< asset-relative .glb/.gltf path
    std::optional<std::string> generator;             ///< kProceduralPrefabKinds entry (e.g. "crt")
    nlohmann::json params = nlohmann::json::object(); ///< generator parameters (object, empty when absent)
    std::optional<glm::vec3> scaleMeters;             ///< nominal size in meters (x, y, z)
    std::vector<std::string> lods;                    ///< LOD model paths, highest detail first
    std::string defaultMaterialVariant;
    std::map<std::string, std::string> sockets; ///< semantic role ("screen", "inspect", ...) -> socket id or node name
    std::string collision;                      ///< collision shape hint ("box", "walls", "proxy", "none", ...)
    std::optional<VideoSurfaceInfo> videoSurface;
};

/// Machine-readable prefab manifest (style §11), extended with "runtime.generator" + "runtime.params" for
/// procedural prefabs:
/// {
///   "schemaVersion": 1, "id": "electronics.crt.consumer.generic", "assetType": "electronic_screen",
///   "category": "crt", "role": "hero",
///   "source": {"kind": "original", "url": null, "author": "...", "acquiredAt": "2026-10-14", "license": "MIT",
///              "licenseSnapshot": null, "attributionRequired": false, "redistributionApproved": true},
///   "runtime": {"model": "models/x.glb" | "generator": "crt", "params": {}, "scaleMeters": [0.71, 0.61, 0.56],
///               "lods": [...], "defaultMaterialVariant": "...", "sockets": {"screen": "ScreenSurface"},
///               "collision": "box", "videoSurface": {"aspectRatio": "4:3", "meshNode": "ScreenSurface"}},
///   "tags": ["crt"], "fallbackPrefab": "electronics.crt.generic", "hash": "sha256:..."
/// }
struct PrefabManifest {
    int schemaVersion = 1;
    std::string id;
    std::string assetType;
    std::string category;
    std::string role;
    SourceInfo source;
    RuntimeInfo runtime;
    std::vector<std::string> tags;
    std::string fallbackPrefab;
    std::string hash;
};

/// Placeholder accepted (with a warning) until the build computes the real digest.
inline constexpr std::string_view kPlaceholderHash = "sha256:TO_BE_GENERATED_BY_BUILD";

/// Procedural prefab kinds of the shared geometry contract (the kinds rv::geometry::buildPrefab builds), the only
/// values accepted for runtime.generator.
inline constexpr std::array<std::string_view, 15> kProceduralPrefabKinds = {
    "room_shell", "shelf",      "cartridge", "crt",          "tv_cabinet", "console_cartridge", "poster_frame",
    "rug",        "floor_lamp", "armchair",  "display_case", "wall_shelf", "storage_box",       "magazine_stack",
    "bookends"};

/// Parses the structure of a manifest: JSON types, unknown keys (errors), required keys ("schemaVersion" = 1,
/// "id", "assetType", "category", "source", "runtime"). Optional strings may be null. Semantic rules are checked by
/// validatePrefabManifest(). Error contexts are JSON paths: "<context>:/runtime/scaleMeters".
[[nodiscard]] Expected<PrefabManifest> parsePrefabManifest(const nlohmann::json& document, std::string_view context);

/// Reads and parses a manifest file (context = UTF-8 path).
[[nodiscard]] Expected<PrefabManifest> loadPrefabManifest(const std::filesystem::path& path);

struct ValidationIssue {
    enum class Severity { Warning, Error };
    Severity severity = Severity::Error;
    /// "<field>: <problem>", e.g. "runtime.sockets.screen: socket \"Screen\" does not exist".
    std::string message;
};

/// Semantic checks (style §11, §26.3). Errors:
/// - id matches ^[a-z0-9_]+([.][a-z0-9_]+)+$; assetType and category are not empty;
/// - exactly one of runtime.model and runtime.generator; model and LOD paths are safe relative paths (model ends in
///   .glb or .gltf); generator is one of kProceduralPrefabKinds;
/// - runtime.scaleMeters components are finite and > 0;
/// - assetType "electronic_screen" requires runtime.sockets.screen and runtime.videoSurface;
/// - videoSurface.aspectRatio is "<w>:<h>" with positive numbers and meshNode is not empty;
/// - when socket sources are given: every runtime.sockets value and videoSurface.meshNode names an existing socket
///   (by id or node name) of `model` or `proceduralSockets`, and the screen socket / meshNode is a ScreenSurface;
/// - source.license is not empty; source.redistributionApproved is true unless source.kind is "user";
/// - hash is "sha256:" followed by 64 hex digits (the kPlaceholderHash placeholder is only a warning);
/// - fallbackPrefab, when present, is a valid id different from id.
/// Warnings: placeholder hash, runtime.params given for a model prefab.
///
/// Socket sources: `model` (null when there is none) and `proceduralSockets`, the complete socket list of the
/// procedural prefab (an empty list means the prefab has no sockets). Without either, socket names are not
/// checked.
[[nodiscard]] std::vector<ValidationIssue>
validatePrefabManifest(const PrefabManifest& manifest, const ModelData* model = nullptr,
                       std::optional<std::span<const geometry::Socket>> proceduralSockets = std::nullopt);

/// True when `issues` contains at least one error.
[[nodiscard]] bool hasErrors(std::span<const ValidationIssue> issues) noexcept;

/// True when `id` matches the prefab id pattern ^[a-z0-9_]+([.][a-z0-9_]+)+$.
[[nodiscard]] bool isValidPrefabId(std::string_view id) noexcept;

} // namespace rv::assets
