#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    /// The catalog database's folder (retroverse.sqlite, its WAL and backups; ADR-0008 §5).
    [[nodiscard]] std::filesystem::path databaseDir() const { return m_userDataRoot / "database"; }
    /// What the settings screen writes (ADR-0018 §2, rv/core/settings_file.hpp): never under configDir(), which stays
    /// the user's.
    [[nodiscard]] std::filesystem::path settingsFile() const { return m_userDataRoot / "settings.json"; }

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

/// True when `child` is `parent` or lies inside it. Both are made absolute against the current directory and
/// normalized as Windows does ("." and "..", separators, a \\?\ prefix dropped), then compared element by element with
/// the file-system casing (fileSystemUpperCase: "C:\Users\X" contains "c:\users\x\es-de"), never Unicode-normalized.
/// Lexical only: links, junctions, subst and mapped drives are not resolved (checkUserDataDirectory() resolves them).
[[nodiscard]] bool pathIsWithin(const std::filesystem::path& child, const std::filesystem::path& parent);

/// The elements of `path` as pathIsWithin() compares them: made absolute, normalized, each upper-cased with the
/// file-system casing (a network path starts with an element of two backslashes); nothing when one cannot be folded.
/// Comparing the elements of many paths with a few folded once is pathIsWithin() without folding those again.
[[nodiscard]] std::optional<std::vector<std::wstring>> foldedPathElements(const std::filesystem::path& path);

/// The data-directory guard of ADR-0008 §6 and §7: Retroverse writes its user data (logs, configuration, the
/// database) nowhere in what it only reads. Refuses `userData` when it is, or lies inside, one of `protectedRoots`
/// (the ES-DE home and install folders, the ROM root, the media root; empty paths are skipped): compared with
/// pathIsWithin(), then again on the paths the file system resolves (the deepest existing part of each path replaced
/// by its final path, GetFinalPathNameByHandleW: links, junctions, subst and mapped drives), so that a data
/// directory reaching a protected folder through a link is refused too. Neither path has to exist; nothing is created
/// or written (folders are opened for their attributes only). Errors: InvalidArgument, naming the protected folder,
/// the data directory as context.
[[nodiscard]] Status checkUserDataDirectory(const std::filesystem::path& userData,
                                            std::span<const std::filesystem::path> protectedRoots);

} // namespace rv::core
