#include <rv/assets/prefab_resolver.hpp>

#include <algorithm>
#include <exception>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "json_util.hpp"
#include "path_util.hpp"

namespace rv::assets {
namespace {

constexpr int kFamiliesSchemaVersion = 1;

/// Logs a fallback's warning the first time this process meets it (the same asset root, id, replacement and reason);
/// later resolutions of it log at debug level, so that a room that several jobs of one run resolve (its capacity, its
/// scene, its persisted start) warns once (style §26.3; I3 step 10c).
void warnFallbackOnce(const std::filesystem::path& assetRoot, std::string_view id, std::string_view replacement,
                      std::string_view reason, const std::string& message) {
    static std::mutex mutex;
    static std::set<std::string, std::less<>> warned;
    std::string key = core::pathToUtf8Lossy(assetRoot);
    for (const std::string_view part : {id, replacement, reason}) {
        key.push_back('\0');
        key.append(part);
    }
    bool first = false;
    {
        const std::lock_guard lock(mutex);
        first = warned.insert(std::move(key)).second;
    }
    if (first) {
        spdlog::warn("{}", message);
    } else {
        spdlog::debug("{} (warned before)", message);
    }
}

Expected<PrefabFamily> parseFamily(const nlohmann::json& entry, const std::string& context) {
    if (auto status = detail::requireObject(entry, context); !status) {
        return propagate(status.error());
    }
    if (auto status = detail::rejectUnknownKeys(entry, {"id", "generic", "generator", "params"}, context); !status) {
        return propagate(status.error());
    }
    PrefabFamily family;
    for (const auto& [key, out] : {std::pair<std::string_view, std::string*>{"id", &family.id},
                                   std::pair<std::string_view, std::string*>{"generic", &family.generic},
                                   std::pair<std::string_view, std::string*>{"generator", &family.generator}}) {
        const auto it = entry.find(key);
        if (it == entry.end()) {
            return makeError(ErrorCode::ParseError, "missing key", detail::pointerChild(context, key));
        }
        auto value = detail::readString(*it, detail::pointerChild(context, key));
        if (!value) {
            return propagate(value.error());
        }
        *out = std::move(*value);
    }
    if (const auto it = entry.find("params"); it != entry.end()) {
        if (auto status = detail::requireObject(*it, detail::pointerChild(context, "params")); !status) {
            return propagate(status.error());
        }
        family.params = *it;
    }
    if (!isValidPrefabId(family.id)) {
        return makeError(ErrorCode::ParseError, "\"" + family.id + "\" is not a valid prefab id",
                         detail::pointerChild(context, "id"));
    }
    if (!isValidPrefabId(family.generic) || !family.generic.starts_with(family.id + ".")) {
        return makeError(ErrorCode::ParseError,
                         "\"" + family.generic + "\" is not the id of a prefab of the family \"" + family.id + "\"",
                         detail::pointerChild(context, "generic"));
    }
    if (std::find(kProceduralPrefabKinds.begin(), kProceduralPrefabKinds.end(), family.generator) ==
        kProceduralPrefabKinds.end()) {
        return makeError(ErrorCode::ParseError, "\"" + family.generator + "\" is not a procedural prefab kind",
                         detail::pointerChild(context, "generator"));
    }
    return family;
}

Expected<PrefabFamilies> parseFamilies(const nlohmann::json& document, std::string_view context) {
    const std::string root = std::string(context) + ":";
    if (auto status = detail::requireObject(document, root + "/"); !status) {
        return propagate(status.error());
    }
    if (auto status = detail::rejectUnknownKeys(document, {"schemaVersion", "families"}, root); !status) {
        return propagate(status.error());
    }
    const auto version = document.find("schemaVersion");
    if (version == document.end()) {
        return makeError(ErrorCode::ParseError, "missing key", detail::pointerChild(root, "schemaVersion"));
    }
    auto number = detail::readInteger(*version, detail::pointerChild(root, "schemaVersion"));
    if (!number) {
        return propagate(number.error());
    }
    if (*number != kFamiliesSchemaVersion) {
        return makeError(ErrorCode::ParseError,
                         "schemaVersion " + std::to_string(*number) + " is not supported (expected " +
                             std::to_string(kFamiliesSchemaVersion) + ")",
                         detail::pointerChild(root, "schemaVersion"));
    }
    const auto list = document.find("families");
    const std::string listContext = detail::pointerChild(root, "families");
    if (list == document.end()) {
        return makeError(ErrorCode::ParseError, "missing key", listContext);
    }
    if (!list->is_array()) {
        return makeError(ErrorCode::ParseError,
                         std::string("expected an array, found ") + std::string(detail::jsonTypeName(*list)),
                         listContext);
    }
    PrefabFamilies families;
    for (std::size_t i = 0; i < list->size(); ++i) {
        auto family = parseFamily((*list)[i], detail::pointerChild(listContext, i));
        if (!family) {
            return propagate(family.error());
        }
        families.families.push_back(std::move(*family));
    }
    std::sort(families.families.begin(), families.families.end(),
              [](const PrefabFamily& a, const PrefabFamily& b) { return a.id < b.id; });
    const auto twice = std::adjacent_find(families.families.begin(), families.families.end(),
                                          [](const PrefabFamily& a, const PrefabFamily& b) { return a.id == b.id; });
    if (twice != families.families.end()) {
        return makeError(ErrorCode::ParseError, "the family \"" + twice->id + "\" is listed twice", listContext);
    }
    return families;
}

/// A manifest found usable, with its procedural prefab built.
struct Usable {
    PrefabManifest manifest;
    std::optional<geometry::ProceduralPrefab> prefab;
};

/// The first error of `issues` as an error value.
tl::unexpected<Error> firstIssue(const std::vector<ValidationIssue>& issues, std::string_view id) {
    for (const ValidationIssue& issue : issues) {
        if (issue.severity == ValidationIssue::Severity::Error) {
            return makeError(ErrorCode::ValidationFailed, issue.message, std::string(id));
        }
    }
    return makeError(ErrorCode::ValidationFailed, "invalid manifest", std::string(id));
}

/// Step 1's rules for one manifest.
Expected<Usable> loadUsable(const std::filesystem::path& assetRoot, std::string_view id) {
    const std::filesystem::path path = prefabManifestPath(assetRoot, id);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return makeError(ErrorCode::NotFound, "the manifest is missing", std::string(id));
    }
    auto manifest = loadPrefabManifest(path);
    if (!manifest) {
        return propagate(manifest.error());
    }
    if (manifest->id != id) {
        return makeError(ErrorCode::ValidationFailed, "the manifest names the id \"" + manifest->id + "\"",
                         std::string(id));
    }
    if (const auto issues = validatePrefabManifest(*manifest); hasErrors(issues)) {
        return firstIssue(issues, id);
    }
    Usable usable{std::move(*manifest), std::nullopt};
    const RuntimeInfo& runtime = usable.manifest.runtime;
    if (runtime.generator) {
        auto prefab = geometry::buildPrefab(*runtime.generator, runtime.params);
        if (!prefab) {
            return propagate(prefab.error());
        }
        const auto issues = validatePrefabManifest(usable.manifest, nullptr,
                                                   std::span<const geometry::Socket>(prefab->sockets));
        if (hasErrors(issues)) {
            return firstIssue(issues, id);
        }
        usable.prefab = std::move(*prefab);
    } else if (runtime.model) {
        auto relative = detail::normalizeRelativePath(*runtime.model, false, "runtime.model");
        if (!relative) {
            return propagate(relative.error());
        }
        if (!std::filesystem::is_regular_file(assetRoot / core::pathFromUtf8(*relative), error)) {
            return makeError(ErrorCode::NotFound, "the model \"" + *relative + "\" is missing", std::string(id));
        }
    }
    return usable;
}

ResolvedPrefab fromManifest(std::string_view requestedId, PrefabSource source, Usable usable) {
    ResolvedPrefab resolved;
    resolved.requestedId = std::string(requestedId);
    resolved.source = source;
    if (usable.manifest.runtime.generator) {
        resolved.generator = *usable.manifest.runtime.generator;
        resolved.params = usable.manifest.runtime.params;
    }
    resolved.prefab = std::move(usable.prefab);
    resolved.manifest = std::move(usable.manifest);
    return resolved;
}

} // namespace

const PrefabFamily* PrefabFamilies::familyOf(std::string_view prefabId) const noexcept {
    const PrefabFamily* best = nullptr;
    for (const PrefabFamily& family : families) {
        if (prefabId.size() > family.id.size() && prefabId.starts_with(family.id) &&
            prefabId[family.id.size()] == '.' && (best == nullptr || family.id.size() > best->id.size())) {
            best = &family;
        }
    }
    return best;
}

Expected<PrefabFamilies> parsePrefabFamilies(const nlohmann::json& document, std::string_view context) {
    try {
        return parseFamilies(document, context);
    } catch (const std::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("unexpected JSON failure: ") + e.what(),
                         std::string(context));
    }
}

Expected<PrefabFamilies> loadPrefabFamilies(const std::filesystem::path& assetRoot) {
    const std::filesystem::path file = assetRoot / core::pathFromUtf8(kPrefabFamiliesFile);
    auto document = core::readJsonFile(file);
    if (!document) {
        return propagate(document.error());
    }
    return parsePrefabFamilies(*document, core::pathToUtf8(file));
}

std::filesystem::path prefabManifestPath(const std::filesystem::path& assetRoot, std::string_view id) {
    return assetRoot / core::pathFromUtf8(kPrefabManifestDir) / core::pathFromUtf8(std::string(id) + ".json");
}

std::string_view toString(PrefabSource source) noexcept {
    switch (source) {
    case PrefabSource::Requested:
        return "requested";
    case PrefabSource::FamilyGeneric:
        return "family generic";
    case PrefabSource::FamilyProcedural:
        return "family procedural";
    }
    return "requested";
}

Expected<ResolvedPrefab> resolvePrefab(const std::filesystem::path& assetRoot, const PrefabFamilies& families,
                                       std::string_view id) {
    if (!isValidPrefabId(id)) {
        return makeError(ErrorCode::InvalidArgument, "not a valid prefab id", std::string(id));
    }
    auto requested = loadUsable(assetRoot, id);
    if (requested) {
        return fromManifest(id, PrefabSource::Requested, std::move(*requested));
    }
    const PrefabFamily* family = families.familyOf(id);
    if (family == nullptr) {
        return propagate(requested.error());
    }
    std::string reason = requested.error().message;
    if (family->generic != id) {
        auto generic = loadUsable(assetRoot, family->generic);
        if (generic) {
            warnFallbackOnce(assetRoot, id, family->generic, reason,
                             fmt::format("Prefab '{}' cannot be used ({}); using its family's generic prefab '{}'", id,
                                         reason, family->generic));
            ResolvedPrefab resolved = fromManifest(id, PrefabSource::FamilyGeneric, std::move(*generic));
            resolved.fallbackReason = std::move(reason);
            return resolved;
        }
        reason += "; nor its family's generic prefab '" + family->generic + "': " + generic.error().message;
    }
    auto prefab = geometry::buildPrefab(family->generator, family->params);
    if (!prefab) {
        return propagate(prefab.error());
    }
    warnFallbackOnce(assetRoot, id, family->generator, reason,
                     fmt::format("Prefab '{}' cannot be used ({}); using its family's procedural prefab '{}'", id,
                                 reason, family->generator));
    ResolvedPrefab resolved;
    resolved.requestedId = std::string(id);
    resolved.source = PrefabSource::FamilyProcedural;
    resolved.generator = family->generator;
    resolved.params = family->params;
    resolved.prefab = std::move(*prefab);
    resolved.fallbackReason = std::move(reason);
    return resolved;
}

} // namespace rv::assets
