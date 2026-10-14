#include <rv/assets/prefab_manifest.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <utility>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

#include "json_util.hpp"
#include "path_util.hpp"

namespace rv::assets {
namespace {

bool isLowerIdentifierChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

bool isHexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isSha256(std::string_view hash) noexcept {
    constexpr std::string_view kPrefix = "sha256:";
    if (!hash.starts_with(kPrefix) || hash.size() != kPrefix.size() + 64) {
        return false;
    }
    for (const char c : hash.substr(kPrefix.size())) {
        if (!isHexDigit(c)) {
            return false;
        }
    }
    return true;
}

bool isPositiveNumber(std::string_view text) noexcept {
    double value = 0.0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [end, error] = std::from_chars(first, last, value, std::chars_format::fixed);
    return !text.empty() && error == std::errc() && end == last && std::isfinite(value) && value > 0.0;
}

/// "4:3", "16:9", "1.85:1".
bool isAspectRatio(std::string_view text) noexcept {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    return isPositiveNumber(text.substr(0, colon)) && isPositiveNumber(text.substr(colon + 1));
}

using detail::readOptional;

/// Optional string that is "set" when present and not null.
Status readOptionalString(const nlohmann::json& object, std::string_view key, std::string_view context,
                          std::optional<std::string>& out) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return ok();
    }
    auto value = detail::readString(*it, detail::pointerChild(context, key));
    if (!value) {
        return propagate(value.error());
    }
    out = std::move(*value);
    return ok();
}

Expected<SourceInfo> parseSource(const nlohmann::json& object, const std::string& context) {
    if (auto status = detail::requireObject(object, context); !status) {
        return propagate(status.error());
    }
    if (auto status = detail::rejectUnknownKeys(object,
                                                {"kind", "url", "author", "acquiredAt", "license", "licenseSnapshot",
                                                 "attributionRequired", "redistributionApproved"},
                                                context);
        !status) {
        return propagate(status.error());
    }
    SourceInfo source;
    const std::pair<std::string_view, std::string*> strings[] = {
        {"kind", &source.kind},       {"url", &source.url},
        {"author", &source.author},   {"acquiredAt", &source.acquiredAt},
        {"license", &source.license}, {"licenseSnapshot", &source.licenseSnapshot},
    };
    for (const auto& [key, field] : strings) {
        if (auto status = readOptional(object, key, context, detail::readNullableString, *field); !status) {
            return propagate(status.error());
        }
    }
    if (auto status =
            readOptional(object, "attributionRequired", context, detail::readBool, source.attributionRequired);
        !status) {
        return propagate(status.error());
    }
    if (auto status =
            readOptional(object, "redistributionApproved", context, detail::readBool, source.redistributionApproved);
        !status) {
        return propagate(status.error());
    }
    return source;
}

Expected<VideoSurfaceInfo> parseVideoSurface(const nlohmann::json& object, const std::string& context) {
    if (auto status = detail::requireObject(object, context); !status) {
        return propagate(status.error());
    }
    if (auto status = detail::rejectUnknownKeys(object, {"aspectRatio", "meshNode"}, context); !status) {
        return propagate(status.error());
    }
    VideoSurfaceInfo surface;
    if (auto status = readOptional(object, "aspectRatio", context, detail::readString, surface.aspectRatio); !status) {
        return propagate(status.error());
    }
    if (auto status = readOptional(object, "meshNode", context, detail::readString, surface.meshNode); !status) {
        return propagate(status.error());
    }
    return surface;
}

Expected<RuntimeInfo> parseRuntime(const nlohmann::json& object, const std::string& context) {
    if (auto status = detail::requireObject(object, context); !status) {
        return propagate(status.error());
    }
    if (auto status = detail::rejectUnknownKeys(object,
                                                {"model", "generator", "params", "scaleMeters", "lods",
                                                 "defaultMaterialVariant", "sockets", "collision", "videoSurface"},
                                                context);
        !status) {
        return propagate(status.error());
    }
    RuntimeInfo runtime;
    if (auto status = readOptionalString(object, "model", context, runtime.model); !status) {
        return propagate(status.error());
    }
    if (auto status = readOptionalString(object, "generator", context, runtime.generator); !status) {
        return propagate(status.error());
    }
    if (const auto it = object.find("params"); it != object.end() && !it->is_null()) {
        if (auto status = detail::requireObject(*it, detail::pointerChild(context, "params")); !status) {
            return propagate(status.error());
        }
        runtime.params = *it;
    }
    if (const auto it = object.find("scaleMeters"); it != object.end() && !it->is_null()) {
        auto scale = core::jsonToVec3(*it, detail::pointerChild(context, "scaleMeters"));
        if (!scale) {
            return propagate(scale.error());
        }
        runtime.scaleMeters = *scale;
    }
    if (auto status = readOptional(object, "lods", context, detail::readStringArray, runtime.lods); !status) {
        return propagate(status.error());
    }
    if (auto status = readOptional(object, "defaultMaterialVariant", context, detail::readNullableString,
                                   runtime.defaultMaterialVariant);
        !status) {
        return propagate(status.error());
    }
    if (const auto it = object.find("sockets"); it != object.end() && !it->is_null()) {
        const std::string where = detail::pointerChild(context, "sockets");
        if (auto status = detail::requireObject(*it, where); !status) {
            return propagate(status.error());
        }
        for (const auto& item : it->items()) {
            auto value = detail::readString(item.value(), detail::pointerChild(where, item.key()));
            if (!value) {
                return propagate(value.error());
            }
            runtime.sockets.emplace(item.key(), std::move(*value));
        }
    }
    if (auto status = readOptional(object, "collision", context, detail::readNullableString, runtime.collision);
        !status) {
        return propagate(status.error());
    }
    if (const auto it = object.find("videoSurface"); it != object.end() && !it->is_null()) {
        auto surface = parseVideoSurface(*it, detail::pointerChild(context, "videoSurface"));
        if (!surface) {
            return propagate(surface.error());
        }
        runtime.videoSurface = std::move(*surface);
    }
    return runtime;
}

Expected<PrefabManifest> parseManifest(const nlohmann::json& document, std::string_view context) {
    const std::string root = std::string(context) + ":";
    if (auto status = detail::requireObject(document, root + "/"); !status) {
        return propagate(status.error());
    }
    if (auto status = detail::rejectUnknownKeys(document,
                                                {"schemaVersion", "id", "assetType", "category", "role", "source",
                                                 "runtime", "tags", "fallbackPrefab", "hash"},
                                                root);
        !status) {
        return propagate(status.error());
    }
    for (const std::string_view key : {"schemaVersion", "id", "assetType", "category", "source", "runtime"}) {
        if (!document.contains(key)) {
            return makeError(ErrorCode::ParseError, "missing required key \"" + std::string(key) + "\"",
                             detail::pointerChild(root, key));
        }
    }

    PrefabManifest manifest;
    const std::string versionContext = detail::pointerChild(root, "schemaVersion");
    auto version = detail::readInteger(document["schemaVersion"], versionContext);
    if (!version) {
        return propagate(version.error());
    }
    if (*version != 1) {
        return makeError(ErrorCode::Unsupported, "unsupported schemaVersion " + std::to_string(*version),
                         versionContext);
    }
    manifest.schemaVersion = *version;

    const std::pair<std::string_view, std::string*> requiredStrings[] = {
        {"id", &manifest.id}, {"assetType", &manifest.assetType}, {"category", &manifest.category}};
    for (const auto& [key, field] : requiredStrings) {
        if (auto status = readOptional(document, key, root, detail::readString, *field); !status) {
            return propagate(status.error());
        }
    }
    const std::pair<std::string_view, std::string*> optionalStrings[] = {
        {"role", &manifest.role}, {"fallbackPrefab", &manifest.fallbackPrefab}, {"hash", &manifest.hash}};
    for (const auto& [key, field] : optionalStrings) {
        if (auto status = readOptional(document, key, root, detail::readNullableString, *field); !status) {
            return propagate(status.error());
        }
    }
    if (auto status = readOptional(document, "tags", root, detail::readStringArray, manifest.tags); !status) {
        return propagate(status.error());
    }

    auto source = parseSource(document["source"], detail::pointerChild(root, "source"));
    if (!source) {
        return propagate(source.error());
    }
    manifest.source = std::move(*source);
    auto runtime = parseRuntime(document["runtime"], detail::pointerChild(root, "runtime"));
    if (!runtime) {
        return propagate(runtime.error());
    }
    manifest.runtime = std::move(*runtime);
    return manifest;
}

const geometry::Socket* findSocketByName(std::string_view name, const ModelData* model,
                                         std::span<const geometry::Socket> proceduralSockets) {
    const auto matches = [name](const geometry::Socket& socket) {
        return socket.id == name || socket.nodeName == name;
    };
    if (model != nullptr) {
        for (const geometry::Socket& socket : model->sockets) {
            if (matches(socket)) {
                return &socket;
            }
        }
    }
    for (const geometry::Socket& socket : proceduralSockets) {
        if (matches(socket)) {
            return &socket;
        }
    }
    return nullptr;
}

bool hasExtension(std::string_view path, std::string_view extension) {
    return path.size() > extension.size() &&
           core::equalsIgnoreCaseAscii(path.substr(path.size() - extension.size()), extension);
}

} // namespace

bool isValidPrefabId(std::string_view id) noexcept {
    std::size_t segments = 0;
    std::size_t segmentLength = 0;
    for (const char c : id) {
        if (c == '.') {
            if (segmentLength == 0) {
                return false;
            }
            ++segments;
            segmentLength = 0;
        } else if (isLowerIdentifierChar(c)) {
            ++segmentLength;
        } else {
            return false;
        }
    }
    return segmentLength > 0 && segments >= 1;
}

Expected<PrefabManifest> parsePrefabManifest(const nlohmann::json& document, std::string_view context) {
    try {
        return parseManifest(document, context);
    } catch (const std::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("unexpected JSON failure: ") + e.what(),
                         std::string(context));
    }
}

Expected<PrefabManifest> loadPrefabManifest(const std::filesystem::path& path) {
    auto document = core::readJsonFile(path);
    if (!document) {
        return propagate(document.error());
    }
    return parsePrefabManifest(*document, core::pathToUtf8(path));
}

bool hasErrors(std::span<const ValidationIssue> issues) noexcept {
    for (const ValidationIssue& issue : issues) {
        if (issue.severity == ValidationIssue::Severity::Error) {
            return true;
        }
    }
    return false;
}

std::vector<ValidationIssue> validatePrefabManifest(const PrefabManifest& manifest, const ModelData* model,
                                                    std::span<const geometry::Socket> proceduralSockets) {
    std::vector<ValidationIssue> issues;
    const auto error = [&issues](std::string_view field, std::string message) {
        issues.push_back({ValidationIssue::Severity::Error, std::string(field) + ": " + message});
    };
    const auto warning = [&issues](std::string_view field, std::string message) {
        issues.push_back({ValidationIssue::Severity::Warning, std::string(field) + ": " + message});
    };

    if (!isValidPrefabId(manifest.id)) {
        error("id", "\"" + manifest.id + "\" does not match ^[a-z0-9_]+([.][a-z0-9_]+)+$");
    }
    if (manifest.assetType.empty()) {
        error("assetType", "must not be empty");
    }
    if (manifest.category.empty()) {
        error("category", "must not be empty");
    }

    const RuntimeInfo& runtime = manifest.runtime;
    if (runtime.model.has_value() == runtime.generator.has_value()) {
        error("runtime", runtime.model ? "\"model\" and \"generator\" are mutually exclusive"
                                       : "one of \"model\" or \"generator\" is required");
    }
    if (runtime.model) {
        if (auto path = detail::normalizeRelativePath(*runtime.model, false, "runtime.model"); !path) {
            error("runtime.model", path.error().message);
        } else if (!hasExtension(*runtime.model, ".glb") && !hasExtension(*runtime.model, ".gltf")) {
            error("runtime.model", "\"" + *runtime.model + "\" is not a .glb or .gltf file");
        }
        if (!runtime.params.empty()) {
            warning("runtime.params", "parameters are ignored for model prefabs");
        }
    }
    if (runtime.generator) {
        const std::string& kind = *runtime.generator;
        if (kind.empty() || !std::all_of(kind.begin(), kind.end(), isLowerIdentifierChar)) {
            error("runtime.generator", "\"" + kind + "\" is not a procedural prefab kind (lowercase identifier)");
        }
    }
    for (std::size_t i = 0; i < runtime.lods.size(); ++i) {
        if (auto path = detail::normalizeRelativePath(runtime.lods[i], false, "runtime.lods"); !path) {
            error("runtime.lods[" + std::to_string(i) + "]", path.error().message);
        }
    }
    if (runtime.scaleMeters) {
        const glm::vec3& scale = *runtime.scaleMeters;
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(scale[c]) || scale[c] <= 0.0f) {
                error("runtime.scaleMeters", "components must be finite and greater than 0");
                break;
            }
        }
    }
    if (manifest.assetType == "electronic_screen") {
        if (!runtime.sockets.contains("screen")) {
            error("runtime.sockets.screen", "required for electronic_screen prefabs");
        }
        if (!runtime.videoSurface) {
            error("runtime.videoSurface", "required for electronic_screen prefabs");
        }
    }
    if (runtime.videoSurface) {
        if (!isAspectRatio(runtime.videoSurface->aspectRatio)) {
            error("runtime.videoSurface.aspectRatio",
                  "expected \"<w>:<h>\" with positive numbers, found \"" + runtime.videoSurface->aspectRatio + "\"");
        }
        if (runtime.videoSurface->meshNode.empty()) {
            error("runtime.videoSurface.meshNode", "must not be empty");
        }
    }

    if (model != nullptr || !proceduralSockets.empty()) {
        const auto checkScreen = [&](std::string_view field, const std::string& name) {
            const geometry::Socket* socket = findSocketByName(name, model, proceduralSockets);
            if (socket == nullptr) {
                error(field, "socket \"" + name + "\" does not exist");
            } else if (socket->kind != geometry::SocketKind::ScreenSurface) {
                error(field, "\"" + name + "\" is not a ScreenSurface socket (kind " +
                                 std::string(geometry::toString(socket->kind)) + ")");
            }
        };
        for (const auto& [role, name] : runtime.sockets) {
            const std::string field = "runtime.sockets." + role;
            if (role == "screen") {
                checkScreen(field, name);
            } else if (findSocketByName(name, model, proceduralSockets) == nullptr) {
                error(field, "socket \"" + name + "\" does not exist");
            }
        }
        if (runtime.videoSurface && !runtime.videoSurface->meshNode.empty()) {
            checkScreen("runtime.videoSurface.meshNode", runtime.videoSurface->meshNode);
        }
    }

    if (manifest.source.kind.empty()) {
        error("source.kind", "must not be empty");
    }
    if (manifest.source.license.empty()) {
        error("source.license", "must not be empty");
    }
    if (!manifest.source.redistributionApproved && manifest.source.kind != "user") {
        error("source.redistributionApproved", "must be true unless source.kind is \"user\"");
    }
    if (manifest.hash == kPlaceholderHash) {
        warning("hash", "placeholder value; the asset build computes the real SHA-256");
    } else if (!isSha256(manifest.hash)) {
        error("hash", "expected \"sha256:\" followed by 64 hexadecimal digits");
    }
    if (!manifest.fallbackPrefab.empty()) {
        if (!isValidPrefabId(manifest.fallbackPrefab)) {
            error("fallbackPrefab", "\"" + manifest.fallbackPrefab + "\" is not a valid prefab id");
        } else if (manifest.fallbackPrefab == manifest.id) {
            error("fallbackPrefab", "must differ from id");
        }
    }
    return issues;
}

} // namespace rv::assets
