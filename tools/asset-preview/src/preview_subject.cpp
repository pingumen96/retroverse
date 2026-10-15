#include <rv/preview/preview_subject.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <glm/glm.hpp>

#include <rv/assets/gltf_loader.hpp>
#include <rv/core/text.hpp>
#include <rv/scene/scene_geometry.hpp>

namespace rv::preview {
namespace {

using Severity = PreviewMessage::Severity;

void addMessage(PreviewSubject& subject, Severity severity, std::string text,
                PreviewMessage::Topic topic = PreviewMessage::Topic::Asset) {
    subject.messages.push_back(PreviewMessage{severity, std::move(text), topic});
}

bool isRegularFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

std::string fileName(const std::filesystem::path& path) {
    return core::pathToUtf8(path.filename());
}

/// Loads the glTF model `file` into `subject`: the model, its sockets and bounds, its warnings as messages.
void loadModelInto(PreviewSubject& subject, const std::filesystem::path& file) {
    subject.modelFile = file;
    auto loaded = assets::loadGltf(file);
    if (!loaded) {
        addMessage(subject, Severity::Error,
                   fmt::format("{} cannot be loaded: {}", core::pathToUtf8(file), loaded.error().toString()));
        subject.placeholder = true;
        return;
    }
    auto model = std::make_shared<const assets::ModelData>(std::move(*loaded));
    for (const std::string& warning : model->warnings) {
        addMessage(subject, Severity::Warning, warning);
    }
    subject.sockets = model->sockets;
    if (model->drawItems.empty()) {
        addMessage(subject, Severity::Error,
                   fmt::format("{} has nothing to draw (no triangle primitive in its scene)", fileName(file)));
        subject.placeholder = true;
    } else {
        subject.bounds = model->bounds;
    }
    subject.model = std::move(model);
}

/// Builds the procedural prefab `kind` with `params` into `subject`.
void buildPrefabInto(PreviewSubject& subject, const std::string& kind, const nlohmann::json& params) {
    subject.prefabKind = kind;
    subject.params = params.is_object() ? params : nlohmann::json::object();
    auto prefab = geometry::buildPrefab(kind, subject.params);
    if (!prefab) {
        addMessage(subject, Severity::Error,
                   fmt::format("{}{} cannot be built: {}", kPrefabPrefix, kind, prefab.error().toString()));
        subject.placeholder = true;
        return;
    }
    subject.bounds = prefab->bounds;
    subject.sockets = prefab->sockets;
    subject.prefab = std::move(*prefab);
}

/// Validates the manifest of `check` against what `subject` shows, and reports the outcome as a message.
void checkManifest(ManifestCheck& check, PreviewSubject& subject) {
    const assets::PrefabManifest& manifest = *check.manifest;
    if (subject.model) {
        check.issues = assets::validatePrefabManifest(manifest, subject.model.get());
    } else if (subject.prefab) {
        check.issues = assets::validatePrefabManifest(manifest, nullptr,
                                                      std::span<const geometry::Socket>(subject.prefab->sockets));
    } else {
        check.issues = assets::validatePrefabManifest(manifest);
    }
    if (!subject.placeholder) {
        if (std::optional<assets::ValidationIssue> scale = scaleIssue(manifest, subject.bounds)) {
            check.issues.push_back(std::move(*scale));
        }
    }
    const std::size_t errors = check.count(assets::ValidationIssue::Severity::Error);
    const std::size_t warnings = check.count(assets::ValidationIssue::Severity::Warning);
    addMessage(subject, errors > 0 ? Severity::Error : (warnings > 0 ? Severity::Warning : Severity::Info),
               fmt::format("Manifest {} ({}): {} error(s), {} warning(s)", manifest.id, fileName(check.path), errors,
                           warnings),
               PreviewMessage::Topic::Manifest);
}

/// Reads the manifest `file` into a check; a failure is an error message.
ManifestCheck readManifest(PreviewSubject& subject, const std::filesystem::path& file) {
    ManifestCheck check;
    check.path = file;
    auto manifest = assets::loadPrefabManifest(file);
    if (!manifest) {
        check.error = manifest.error();
        addMessage(subject, Severity::Error,
                   fmt::format("Manifest {} cannot be read: {}", fileName(file), manifest.error().toString()),
                   PreviewMessage::Topic::Manifest);
        return check;
    }
    check.manifest = std::move(*manifest);
    return check;
}

void loadModelTarget(PreviewSubject& subject, const std::filesystem::path& file) {
    subject.title = fileName(file);
    subject.watchedFiles.push_back(file);
    loadModelInto(subject, file);
    for (const std::filesystem::path& candidate : sidecarManifestPaths(file)) {
        subject.watchedFiles.push_back(candidate);
        if (!subject.manifest && isRegularFile(candidate)) {
            ManifestCheck check = readManifest(subject, candidate);
            if (check.manifest) {
                checkManifest(check, subject);
            }
            subject.manifest = std::move(check);
        }
    }
}

void loadManifestTarget(PreviewSubject& subject, const std::filesystem::path& file,
                        const std::filesystem::path& assetRoot) {
    subject.title = fileName(file);
    subject.watchedFiles.push_back(file);
    ManifestCheck check = readManifest(subject, file);
    if (!check.manifest) {
        subject.placeholder = true;
        subject.manifest = std::move(check);
        return;
    }
    const assets::PrefabManifest& manifest = *check.manifest;
    if (!manifest.id.empty()) {
        subject.title = manifest.id;
    }
    if (manifest.runtime.model) {
        if (const std::optional<std::filesystem::path> model =
                resolveManifestModel(*manifest.runtime.model, file, assetRoot)) {
            subject.watchedFiles.push_back(*model);
            loadModelInto(subject, *model);
        } else {
            addMessage(subject, Severity::Error,
                       fmt::format("runtime.model \"{}\" is neither under the asset root ({}) nor next to the manifest",
                                   *manifest.runtime.model, core::pathToUtf8(assetRoot)));
            subject.placeholder = true;
        }
    } else if (manifest.runtime.generator) {
        buildPrefabInto(subject, *manifest.runtime.generator, manifest.runtime.params);
    } else {
        subject.placeholder = true; // the checks name the missing runtime
    }
    checkManifest(check, subject);
    subject.manifest = std::move(check);
}

} // namespace

std::size_t ManifestCheck::count(assets::ValidationIssue::Severity severity) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        issues.begin(), issues.end(), [severity](const auto& issue) { return issue.severity == severity; }));
}

std::size_t PreviewSubject::count(PreviewMessage::Severity severity,
                                  std::optional<PreviewMessage::Topic> topic) const noexcept {
    return static_cast<std::size_t>(std::count_if(messages.begin(), messages.end(), [&](const auto& message) {
        return message.severity == severity && (!topic || message.topic == *topic);
    }));
}

std::array<std::filesystem::path, 2> sidecarManifestPaths(const std::filesystem::path& model) {
    const std::filesystem::path directory = model.parent_path();
    const std::string stem = core::pathToUtf8(model.stem());
    return {directory / core::pathFromUtf8(stem + ".manifest.json"), directory / core::pathFromUtf8(stem + ".json")};
}

std::optional<std::filesystem::path> resolveManifestModel(const std::string& model,
                                                          const std::filesystem::path& manifestFile,
                                                          const std::filesystem::path& assetRoot) {
    const std::filesystem::path relative = core::pathFromUtf8(model);
    if (model.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        return std::nullopt;
    }
    for (const std::filesystem::path& component : relative) {
        if (component == "..") {
            return std::nullopt; // validatePrefabManifest reports the unsafe path
        }
    }
    for (const std::filesystem::path& base : {assetRoot, manifestFile.parent_path()}) {
        const std::filesystem::path candidate = (base / relative).lexically_normal();
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<assets::ValidationIssue> scaleIssue(const assets::PrefabManifest& manifest, const core::Aabb& measured) {
    if (!manifest.runtime.scaleMeters || measured.isEmpty()) {
        return std::nullopt;
    }
    const glm::vec3 nominal = *manifest.runtime.scaleMeters;
    const glm::vec3 size = measured.size();
    constexpr std::string_view kAxes = "xyz";
    float worst = 0.0f;
    std::size_t worstAxis = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto c = static_cast<glm::length_t>(axis);
        if (!std::isfinite(nominal[c]) || nominal[c] <= 0.0f || !std::isfinite(size[c])) {
            continue; // validatePrefabManifest reports a bad nominal size
        }
        const float difference = std::abs(size[c] - nominal[c]) / nominal[c];
        if (difference > worst) {
            worst = difference;
            worstAxis = axis;
        }
    }
    if (worst <= kScaleTolerance) {
        return std::nullopt;
    }
    return assets::ValidationIssue{
        assets::ValidationIssue::Severity::Warning,
        fmt::format("runtime.scaleMeters: {:.3f} x {:.3f} x {:.3f} m, the asset measures {:.3f} x {:.3f} x {:.3f} m "
                    "({} differs by {:.0f}%, more than {:.0f}%)",
                    nominal.x, nominal.y, nominal.z, size.x, size.y, size.z, kAxes[worstAxis], 100.0f * worst,
                    100.0f * kScaleTolerance)};
}

PreviewSubject loadPreviewSubject(const PreviewTarget& target, const std::filesystem::path& assetRoot) {
    PreviewSubject subject;
    subject.target = target;
    subject.bounds = scene::placeholderBounds();
    switch (target.kind) {
    case PreviewTarget::Kind::Model:
        loadModelTarget(subject, target.file);
        break;
    case PreviewTarget::Kind::Manifest:
        loadManifestTarget(subject, target.file, assetRoot);
        break;
    case PreviewTarget::Kind::Prefab:
        subject.title = target.label();
        buildPrefabInto(subject, target.prefabKind, target.params);
        break;
    }
    if (subject.placeholder) {
        subject.bounds = scene::placeholderBounds();
    }
    return subject;
}

} // namespace rv::preview
