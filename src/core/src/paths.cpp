#include <rv/core/paths.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "path_elements.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rv::core {
namespace {

std::optional<std::filesystem::path> environmentPath(const char* name) {
#if defined(_WIN32)
    const std::wstring wideName(name, name + std::char_traits<char>::length(name));
    const DWORD required = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(wideName.c_str(), value.data(), required);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    if (value.empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(value);
#endif
}

bool isDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    return absolute.lexically_normal();
}

} // namespace

std::string_view toString(AssetRootSource source) noexcept {
    switch (source) {
    case AssetRootSource::CommandLine:
        return "command line";
    case AssetRootSource::Environment:
        return "RV_ASSET_ROOT";
    case AssetRootSource::SourceTree:
        return "source tree";
    case AssetRootSource::ExecutableDir:
        return "executable directory";
    }
    return "unknown";
}

std::filesystem::path executablePath() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

PathEnvironment PathEnvironment::fromProcess() {
    PathEnvironment environment;
    environment.executableDir = executablePath().parent_path();
    environment.envAssetRoot = environmentPath("RV_ASSET_ROOT");
    environment.localAppData = environmentPath("LOCALAPPDATA");
#if defined(RV_SOURCE_ASSET_DIR)
    environment.sourceAssetDir = pathFromUtf8(RV_SOURCE_ASSET_DIR);
#endif
    return environment;
}

Expected<Paths> Paths::resolve(const PathOptions& options, const PathEnvironment& environment) {
    Paths paths;
    paths.m_executableDir = normalized(environment.executableDir);
    paths.m_portable = options.portable;

    // --- Asset root -------------------------------------------------------------------------------
    if (options.assetRootOverride) {
        if (!isDirectory(*options.assetRootOverride)) {
            return makeError(ErrorCode::NotFound, "--asset-root does not name a directory",
                             pathToUtf8Lossy(*options.assetRootOverride));
        }
        paths.m_assetRoot = normalized(*options.assetRootOverride);
        paths.m_assetRootSource = AssetRootSource::CommandLine;
    } else if (environment.envAssetRoot) {
        if (!isDirectory(*environment.envAssetRoot)) {
            return makeError(ErrorCode::NotFound, "RV_ASSET_ROOT does not name a directory",
                             pathToUtf8Lossy(*environment.envAssetRoot));
        }
        paths.m_assetRoot = normalized(*environment.envAssetRoot);
        paths.m_assetRootSource = AssetRootSource::Environment;
    } else if (environment.sourceAssetDir && isDirectory(*environment.sourceAssetDir)) {
        paths.m_assetRoot = normalized(*environment.sourceAssetDir);
        paths.m_assetRootSource = AssetRootSource::SourceTree;
    } else if (isDirectory(paths.m_executableDir / "assets")) {
        paths.m_assetRoot = paths.m_executableDir / "assets";
        paths.m_assetRootSource = AssetRootSource::ExecutableDir;
    } else {
        std::string searched = pathToUtf8Lossy(paths.m_executableDir / "assets");
        if (environment.sourceAssetDir) {
            searched += ", " + pathToUtf8Lossy(*environment.sourceAssetDir);
        }
        return makeError(ErrorCode::NotFound, "no asset directory found (use --asset-root or RV_ASSET_ROOT)",
                         searched);
    }

    // --- User data root ---------------------------------------------------------------------------
    if (options.userDataOverride) {
        paths.m_userDataRoot = normalized(*options.userDataOverride);
    } else if (options.portable || !environment.localAppData) {
        paths.m_userDataRoot = paths.m_executableDir / "RetroverseData";
    } else {
        paths.m_userDataRoot = normalized(*environment.localAppData / "Retroverse");
    }

    return paths;
}

Expected<std::filesystem::path> Paths::asset(std::string_view relativeUtf8) const {
    std::string text(relativeUtf8);
    for (char& c : text) {
        if (c == '\\') {
            c = '/';
        }
    }
    auto converted = tryPathFromUtf8(text);
    if (!converted) {
        // The text itself stays out of the message: it is not UTF-8.
        return makeError(ErrorCode::InvalidArgument, "asset paths must be UTF-8: " + converted.error().message);
    }
    const std::filesystem::path relative = converted->lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        return makeError(ErrorCode::InvalidArgument, "asset paths must be relative to the asset root",
                         std::string(relativeUtf8));
    }
    const auto first = relative.begin();
    if (first != relative.end() && *first == "..") {
        return makeError(ErrorCode::InvalidArgument, "asset path escapes the asset root", std::string(relativeUtf8));
    }
    return m_assetRoot / relative;
}

namespace detail {

std::optional<std::vector<std::wstring>> foldedPathElements(const std::filesystem::path& path) {
    std::wstring text = extendedLengthPath(path);
    std::vector<std::wstring> elements;
    if (text.starts_with(L"\\\\?\\UNC\\")) {
        text = text.substr(8);
        elements.emplace_back(L"\\\\");
    } else if (text.starts_with(L"\\\\?\\")) {
        text = text.substr(4);
    }
    std::replace(text.begin(), text.end(), L'/', L'\\');
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(L'\\', start);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        if (end > start) {
            auto upper = fileSystemUpperCase(std::wstring_view(text).substr(start, end - start));
            if (!upper) {
                return std::nullopt;
            }
            elements.push_back(std::move(*upper));
        }
        start = end + 1;
    }
    return elements;
}

} // namespace detail

bool pathIsWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto childElements = detail::foldedPathElements(child);
    const auto parentElements = detail::foldedPathElements(parent);
    if (!childElements || !parentElements || parentElements->empty() ||
        parentElements->size() > childElements->size()) {
        return false;
    }
    return std::equal(parentElements->begin(), parentElements->end(), childElements->begin());
}

namespace {

#if defined(_WIN32)
/// The final path of what `path` (\\?\ form) names, links, junctions, subst and mapped drives resolved
/// (GetFinalPathNameByHandleW: "\\?\C:\..." or "\\?\UNC\server\share\..."); nothing when it cannot be opened. Opened
/// for its attributes only: nothing is read or changed.
std::optional<std::wstring> openedFinalPath(const std::wstring& path) {
    // A drive's root folder is "\\?\C:\" ("\\?\C:" would be the volume).
    const std::wstring openable = path.ends_with(L':') ? path + L'\\' : path;
    const HANDLE handle =
        CreateFileW(openable.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::wstring resolved(MAX_PATH, L'\0');
    DWORD length = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        length = GetFinalPathNameByHandleW(handle, resolved.data(), static_cast<DWORD>(resolved.size()),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length < resolved.size()) {
            break;
        }
        resolved.resize(length + 1); // the size it needs, its terminator included
    }
    CloseHandle(handle);
    if (length == 0 || length >= resolved.size()) {
        return std::nullopt;
    }
    resolved.resize(length);
    return resolved;
}
#endif

/// `path` as the file system resolves it: made absolute and normalized (extendedLengthPath), then the deepest part of
/// it that exists replaced by its final path, the rest appended as it is. The lexical form when no part can be
/// resolved (and off Windows).
std::filesystem::path resolvedPath(const std::filesystem::path& path) {
    const std::wstring full = extendedLengthPath(path);
#if defined(_WIN32)
    // The elements that may be taken off: never the drive ("\\?\C:") or the share ("\\?\UNC\server\share").
    std::size_t rootEnd = full.find(L':') == 5 ? 6 : full.size();
    if (full.starts_with(L"\\\\?\\UNC\\")) {
        const std::size_t server = full.find(L'\\', 8);
        rootEnd = server == std::wstring::npos ? full.size() : full.find(L'\\', server + 1);
        rootEnd = rootEnd == std::wstring::npos ? full.size() : rootEnd;
    }
    std::wstring existing = full;
    while (!existing.empty()) {
        if (auto finalPath = openedFinalPath(existing)) {
            std::wstring rest = full.substr(existing.size());
            if (finalPath->ends_with(L'\\') && rest.starts_with(L'\\')) {
                rest.erase(0, 1); // a drive's root folder ends with its separator
            }
            return std::filesystem::path(*finalPath + rest);
        }
        const std::size_t separator = existing.find_last_of(L'\\');
        if (separator == std::wstring::npos || separator < rootEnd || existing.size() <= rootEnd) {
            break;
        }
        existing.resize(separator);
    }
#endif
    return std::filesystem::path(full);
}

} // namespace

Status checkUserDataDirectory(const std::filesystem::path& userData,
                              std::span<const std::filesystem::path> protectedRoots) {
    const std::filesystem::path resolvedData = resolvedPath(userData);
    for (const std::filesystem::path& root : protectedRoots) {
        if (root.empty()) {
            continue;
        }
        if (pathIsWithin(userData, root) || pathIsWithin(resolvedData, resolvedPath(root))) {
            return makeError(ErrorCode::InvalidArgument,
                             "the user data directory lies inside " + pathToUtf8Lossy(root) +
                                 ", which Retroverse only reads: choose another one (--data-dir, --portable)",
                             pathToUtf8Lossy(userData));
        }
    }
    return ok();
}

} // namespace rv::core
