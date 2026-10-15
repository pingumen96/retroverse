#pragma once

// GPU-free (part of rv_preview_core): no SDL, OpenGL or Dear ImGui header is included.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/model_data.hpp>
#include <rv/assets/prefab_manifest.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/preview/preview_options.hpp>

namespace rv::preview {

/// A line of the Warnings panel (also logged at its level).
struct PreviewMessage {
    enum class Severity : std::uint8_t { Info, Warning, Error };
    /// What it is about: the asset (loading the model, building the prefab) or its manifest (reading it, the summary of
    /// its checks, whose details are ManifestCheck::issues).
    enum class Topic : std::uint8_t { Asset, Manifest };

    Severity severity = Severity::Info;
    std::string text;
    Topic topic = Topic::Asset;
};

/// Largest difference between a manifest's nominal size (runtime.scaleMeters) and the measured bounds, per axis, as a
/// share of the nominal size, before scaleIssue() warns.
inline constexpr float kScaleTolerance = 0.05f;

/// A prefab manifest of the subject (the target itself, or a manifest found next to the model) and its checks.
struct ManifestCheck {
    std::filesystem::path path;
    /// Empty when the file could not be read or parsed (`error` says why).
    std::optional<assets::PrefabManifest> manifest;
    std::optional<Error> error;
    /// assets::validatePrefabManifest against the sockets of the subject (the loaded model, the built prefab; none
    /// when neither loaded), then scaleIssue() against its measured bounds.
    std::vector<assets::ValidationIssue> issues;

    [[nodiscard]] std::size_t count(assets::ValidationIssue::Severity severity) const noexcept;
};

/// What the asset preview shows, loaded from a PreviewTarget. GPU-free: the tool turns it into a scene.
struct PreviewSubject {
    /// None: an empty preview (no target yet).
    std::optional<PreviewTarget> target;
    /// Short name for titles: the file name, the manifest's id, "prefab:<kind>".
    std::string title;
    /// The glTF model shown: a Model target, or the runtime.model of a Manifest target. Null otherwise and when it
    /// could not be loaded.
    std::shared_ptr<const assets::ModelData> model;
    /// The model's file (as resolved for a manifest); empty without a model to load.
    std::filesystem::path modelFile;
    /// The procedural prefab shown: a Prefab target, or the runtime.generator of a Manifest target (with its
    /// runtime.params). Empty otherwise and when the builder refused its parameters.
    std::optional<geometry::ProceduralPrefab> prefab;
    /// The prefab's kind and parameters, also when it could not be built (the scene then shows the placeholder).
    std::string prefabKind;
    nlohmann::json params = nlohmann::json::object();
    /// The manifest loaded as the target or found next to the model (sidecarManifestPaths), with its checks.
    std::optional<ManifestCheck> manifest;
    /// Bounds of what is shown: the model's or the prefab's, else those of the magenta placeholder.
    core::Aabb bounds;
    /// Sockets of the model or of the prefab.
    std::vector<geometry::Socket> sockets;
    /// The loader's warnings and what the preview could not do, in order.
    std::vector<PreviewMessage> messages;
    /// Nothing of the target could be loaded or built: the scene shows the magenta placeholder (style §21).
    bool placeholder = false;
    /// Files whose changes reload the subject: the target, the manifest's model, the manifest files that would sit next
    /// to a model (also those that do not exist yet, so that creating one is noticed).
    std::vector<std::filesystem::path> watchedFiles;

    /// Messages of `severity`, of every topic or of `topic` only.
    [[nodiscard]] std::size_t count(PreviewMessage::Severity severity,
                                    std::optional<PreviewMessage::Topic> topic = std::nullopt) const noexcept;
};

/// Manifest files that describe the model `model` when they sit next to it, in the order they are looked for:
/// "<stem>.manifest.json", then "<stem>.json".
[[nodiscard]] std::array<std::filesystem::path, 2> sidecarManifestPaths(const std::filesystem::path& model);

/// The model file a manifest's runtime.model names: under `assetRoot` (where shipped manifests point), else next to
/// the manifest (a manifest kept beside a model being prepared). Nothing when neither exists.
[[nodiscard]] std::optional<std::filesystem::path> resolveManifestModel(const std::string& model,
                                                                        const std::filesystem::path& manifestFile,
                                                                        const std::filesystem::path& assetRoot);

/// A warning when the manifest's nominal size (runtime.scaleMeters) and the `measured` bounds differ by more than
/// kScaleTolerance on an axis (a model exported in the wrong unit, a wrong manifest): "runtime.scaleMeters: 0.71 x
/// 0.61 x 0.56 m, the model measures ...". Nothing without a nominal size or with empty bounds.
[[nodiscard]] std::optional<assets::ValidationIssue> scaleIssue(const assets::PrefabManifest& manifest,
                                                                const core::Aabb& measured);

/// Loads `target`, never failing: what cannot be loaded becomes a message and the placeholder.
///  - Model: assets::loadGltf (its warnings become messages); a model without drawn primitives shows the placeholder.
///    A manifest next to it (sidecarManifestPaths) is checked against it.
///  - Manifest: assets::loadPrefabManifest; its runtime.model is loaded like a model (resolveManifestModel against
///    `assetRoot`), its runtime.generator built like a prefab with runtime.params; then it is checked.
///  - Prefab: geometry::buildPrefab with the target's parameters (a refusal is an error message naming the parameter).
[[nodiscard]] PreviewSubject loadPreviewSubject(const PreviewTarget& target, const std::filesystem::path& assetRoot);

} // namespace rv::preview
