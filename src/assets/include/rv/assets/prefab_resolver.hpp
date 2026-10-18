#pragma once

// Object fallbacks (style §7.3, §21, §26.3; I3 step 1b): prefab families, and the resolution of a prefab id to a
// usable manifest, else its family's generic manifest, else its family's procedural prefab.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/prefab_manifest.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/prefabs.hpp>

namespace rv::assets {

/// The family registry and the manifests, relative to the asset root.
inline constexpr std::string_view kPrefabFamiliesFile = "manifests/families.json";
inline constexpr std::string_view kPrefabManifestDir = "manifests/prefabs";

/// A family of prefabs (style §7.3's tier B and C): the prefabs whose ids start with `id` and a dot.
struct PrefabFamily {
    /// A valid prefab id itself ("furniture.shelf", "media.cartridge").
    std::string id;
    /// The family's generic manifest (tier B), a member of the family.
    std::string generic;
    /// The procedural prefab built when even the generic manifest cannot be used (tier C): a kProceduralPrefabKinds
    /// kind and its parameters (an object), the generic manifest's own.
    std::string generator;
    nlohmann::json params = nlohmann::json::object();
};

struct PrefabFamilies {
    /// Sorted by id, ids unique.
    std::vector<PrefabFamily> families;

    /// The family whose id is the longest prefix of `prefabId` followed by a dot, or nullptr.
    [[nodiscard]] const PrefabFamily* familyOf(std::string_view prefabId) const noexcept;
};

/// Parses the registry: {"schemaVersion": 1, "families": [{"id", "generic", "generator", "params"}, ...]} ("params"
/// optional, an object). Errors (ParseError, context "<context>:/families/<i>/<key>"): unknown keys, a missing key, a
/// newer schemaVersion, an invalid id, a generic that is not a valid id of the family, a generator that is not a
/// procedural kind, a family listed twice.
[[nodiscard]] Expected<PrefabFamilies> parsePrefabFamilies(const nlohmann::json& document, std::string_view context);

/// Reads <assetRoot>/manifests/families.json.
[[nodiscard]] Expected<PrefabFamilies> loadPrefabFamilies(const std::filesystem::path& assetRoot);

/// <assetRoot>/manifests/prefabs/<id>.json (`id` must be a valid prefab id).
[[nodiscard]] std::filesystem::path prefabManifestPath(const std::filesystem::path& assetRoot, std::string_view id);

/// Which tier a resolution ended on.
enum class PrefabSource {
    Requested,        ///< the requested manifest
    FamilyGeneric,    ///< the family's generic manifest (tier B)
    FamilyProcedural, ///< the family's procedural prefab (tier C), no manifest
};

[[nodiscard]] std::string_view toString(PrefabSource source) noexcept;

struct ResolvedPrefab {
    std::string requestedId;
    PrefabSource source = PrefabSource::Requested;
    /// The manifest used (Requested, FamilyGeneric); nullopt for FamilyProcedural.
    std::optional<PrefabManifest> manifest;
    /// Procedural source: the generator and its parameters (the manifest's, or the family's for FamilyProcedural);
    /// empty for a model manifest. A caller that sizes the prefab (shelf dressing to a level) builds it again from
    /// these with its own keys merged in.
    std::string generator;
    nlohmann::json params = nlohmann::json::object();
    /// The procedural prefab built from generator and params; nullopt for a model manifest (the caller loads
    /// runtime.model, and when that fails falls back the same way, style §21: "GLB malformato").
    std::optional<geometry::ProceduralPrefab> prefab;
    /// Why the requested manifest (and the generic one) could not be used; empty for Requested.
    std::string fallbackReason;
};

/// Resolves a prefab id (style §7.3's algorithm, with the families above):
///  1. the manifest <assetRoot>/manifests/prefabs/<id>.json when it is usable: it loads and parses, names `id`,
///     validatePrefabManifest reports no error (warnings, such as the placeholder hash, are fine), and a procedural
///     one builds with its parameters and has every socket its runtime.sockets name (a model one: its file exists
///     under the asset root);
///  2. else the generic manifest of the id's family, under the same rules (skipped when it is the requested one);
///  3. else the family's procedural prefab.
/// A fallback logs exactly one warning naming the id, why it could not be used and what replaces it; the requested
/// manifest used as it is logs nothing. Errors: an invalid id (InvalidArgument); an unusable manifest with no family
/// (the reason, as its error); a family whose procedural prefab does not build (its error). Nothing is cached: the
/// caller keeps what it resolved.
[[nodiscard]] Expected<ResolvedPrefab> resolvePrefab(const std::filesystem::path& assetRoot,
                                                     const PrefabFamilies& families, std::string_view id);

} // namespace rv::assets
