#include <rv/core/paths.hpp>

#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#include <rv/core/text.hpp>

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
                             pathToUtf8(*options.assetRootOverride));
        }
        paths.m_assetRoot = normalized(*options.assetRootOverride);
        paths.m_assetRootSource = AssetRootSource::CommandLine;
    } else if (environment.envAssetRoot) {
        if (!isDirectory(*environment.envAssetRoot)) {
            return makeError(ErrorCode::NotFound, "RV_ASSET_ROOT does not name a directory",
                             pathToUtf8(*environment.envAssetRoot));
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
        std::string searched = pathToUtf8(paths.m_executableDir / "assets");
        if (environment.sourceAssetDir) {
            searched += ", " + pathToUtf8(*environment.sourceAssetDir);
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
    const std::filesystem::path relative = pathFromUtf8(text).lexically_normal();
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

} // namespace rv::core
