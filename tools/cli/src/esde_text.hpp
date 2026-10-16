#pragma once

// Private to rv_cli_core (I2 step 2): what `discover` and `library` share: the folder options, and how folders, roots
// and issues read as text and as JSON.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/catalog/snapshot.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/settings.hpp>

namespace rv::cli {

using Json = nlohmann::ordered_json;

/// An absolute folder from a path option (relative ones against the current folder), tidied as discovery and
/// library.json keep folders (esde::folderFromEsdeText). Errors (InvalidArgument, naming the option): an empty value,
/// text that is not UTF-8, a path that cannot be made absolute.
inline Expected<std::optional<std::filesystem::path>> folderOption(const core::ParsedArgs& args,
                                                                   std::string_view name) {
    if (!args.has(name)) {
        return std::optional<std::filesystem::path>{};
    }
    const std::string option = "--" + std::string(name);
    const std::string text = args.value(name).value_or("");
    if (text.empty()) {
        return makeError(ErrorCode::InvalidArgument, "value must not be empty", option);
    }
    auto path = core::tryPathFromUtf8(text);
    if (!path) {
        return makeError(ErrorCode::InvalidArgument, "value is not valid UTF-8", option);
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(*path, error);
    auto folder = error ? std::nullopt : esde::folderFromEsdeText(core::pathToUtf8Lossy(absolute));
    if (!folder) {
        return makeError(ErrorCode::InvalidArgument, "cannot be made an absolute folder", option);
    }
    return std::optional<std::filesystem::path>(std::move(*folder));
}

/// A path as the results show it (UTF-8, unpaired surrogates as U+FFFD).
inline std::string pathText(const std::filesystem::path& path) {
    return core::pathToUtf8Lossy(path);
}

/// A path as a JSON string, or null.
inline Json pathJson(const std::optional<std::filesystem::path>& path) {
    return path ? Json(pathText(*path)) : Json(nullptr);
}

/// A root and where it came from: "<folder> (ROMDirectory \"<value>\")", "<folder> (override)", "<folder> (default:
/// ROMDirectory is empty)", "unresolved (...)"; `unknown` when there is no root at all (no settings).
inline std::string rootText(const std::optional<esde::ResolvedRoot>& root, std::string_view setting,
                            std::string_view unknown) {
    if (!root) {
        return std::string(unknown);
    }
    if (!root->path) {
        return fmt::format("unresolved ({} \"{}\")", setting, root->configured);
    }
    switch (root->source) {
    case esde::RootSource::Override:
        return fmt::format("{} (override)", pathText(*root->path));
    case esde::RootSource::Setting:
        return fmt::format("{} ({} \"{}\")", pathText(*root->path), setting, root->configured);
    case esde::RootSource::Default:
        return fmt::format("{} (default: {} is empty)", pathText(*root->path), setting);
    }
    return pathText(*root->path);
}

/// A root as JSON ({"path", "source", "configured"}), or null.
inline Json rootJson(const std::optional<esde::ResolvedRoot>& root) {
    if (!root) {
        return nullptr;
    }
    return Json{{"path", pathJson(root->path)},
                {"source", std::string(esde::toString(root->source))},
                {"configured", root->configured}};
}

/// An issue as JSON.
inline Json issueJson(const catalog::Issue& issue) {
    return Json{{"severity", std::string(catalog::toString(issue.severity))},
                {"code", issue.code},
                {"system", issue.system},
                {"file", issue.file},
                {"reason", issue.reason}};
}

} // namespace rv::cli
