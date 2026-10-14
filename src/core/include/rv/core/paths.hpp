#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::core {

/// Process-level facts used to resolve directories. Injectable so path resolution is unit-testable.
struct PathEnvironment {
    std::filesystem::path executableDir;
    std::optional<std::filesystem::path> envAssetRoot;   ///< RV_ASSET_ROOT environment variable
    std::optional<std::filesystem::path> localAppData;   ///< %LOCALAPPDATA%
    std::optional<std::filesystem::path> sourceAssetDir; ///< <source>/assets, compiled in for dev builds only

    /// Reads the real process environment.
    [[nodiscard]] static PathEnvironment fromProcess();
};

/// User choices that influence path resolution (command line).
struct PathOptions {
    std::optional<std::filesystem::path> assetRootOverride; ///< --asset-root
    std::optional<std::filesystem::path> userDataOverride;  ///< --data-dir
    bool portable = false;                                  ///< --portable: data next to the executable
};

enum class AssetRootSource { CommandLine, Environment, SourceTree, ExecutableDir };

[[nodiscard]] std::string_view toString(AssetRootSource source) noexcept;

/// Resolved application directories (specification §16.1).
///
/// Asset root precedence: --asset-root, RV_ASSET_ROOT, <source>/assets (dev builds), <exe>/assets.
/// User data root: --data-dir, <exe>/RetroverseData when portable, %LOCALAPPDATA%/Retroverse otherwise.
/// Resolution never creates directories.
class Paths {
public:
    [[nodiscard]] static Expected<Paths> resolve(const PathOptions& options, const PathEnvironment& environment);

    [[nodiscard]] const std::filesystem::path& executableDir() const noexcept { return m_executableDir; }
    [[nodiscard]] const std::filesystem::path& assetRoot() const noexcept { return m_assetRoot; }
    [[nodiscard]] AssetRootSource assetRootSource() const noexcept { return m_assetRootSource; }
    [[nodiscard]] const std::filesystem::path& userDataRoot() const noexcept { return m_userDataRoot; }
    [[nodiscard]] bool isPortable() const noexcept { return m_portable; }

    [[nodiscard]] std::filesystem::path logsDir() const { return m_userDataRoot / "logs"; }
    [[nodiscard]] std::filesystem::path configDir() const { return m_userDataRoot / "config"; }
    [[nodiscard]] std::filesystem::path cacheDir() const { return m_userDataRoot / "cache"; }
    [[nodiscard]] std::filesystem::path screenshotsDir() const { return m_userDataRoot / "screenshots"; }

    /// Resolves an asset-relative UTF-8 path ("shaders/pbr/pbr_base.frag"). Forward or back slashes are
    /// accepted; absolute paths and paths escaping the asset root ("..") are rejected.
    [[nodiscard]] Expected<std::filesystem::path> asset(std::string_view relativeUtf8) const;

private:
    std::filesystem::path m_executableDir;
    std::filesystem::path m_assetRoot;
    AssetRootSource m_assetRootSource = AssetRootSource::ExecutableDir;
    std::filesystem::path m_userDataRoot;
    bool m_portable = false;
};

/// Absolute path of the running executable.
[[nodiscard]] std::filesystem::path executablePath();

} // namespace rv::core
