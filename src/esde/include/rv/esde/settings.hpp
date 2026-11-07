#pragma once

// The settings Retroverse takes from ES-DE's es_settings.xml (ADR-0008 §3, §7), and only them: the file also holds the
// ScreenScraper user name and password in plain text, so it is read through a whitelist of keys (settingKeys()); no
// other key's value is ever extracted, logged, copied or returned. The file has no root element: ES-DE writes one
// <bool>, <int>, <float> or <string> element per setting, each with a name and a value attribute, and reads them as
// ES-DE 3.4.1's Settings::loadFile does (the last of two elements of one key wins, a key under another element type is
// not that setting, a boolean is true when its value starts with 1, t, T, y or Y, a missing setting keeps ES-DE's
// default). The ROM and media roots are resolved as ES-DE's FileData::getROMDirectory and getMediaDirectory resolve
// them: '~' (every one of them) is ES-DE's home folder, the parent of the ES-DE home; %ESPATH% is the install folder;
// an empty value is ES-DE's default.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>

namespace rv::esde {

/// ES-DE's user interface mode (UIMode).
enum class UiMode : std::uint8_t {
    Full,  ///< "full"
    Kiosk, ///< "kiosk": no configuration
    Kid,   ///< "kid": only the games marked <kidgame>
};

/// "full", "kiosk", "kid".
[[nodiscard]] std::string_view toString(UiMode mode) noexcept;

/// Where the settings came from.
enum class SettingsSource : std::uint8_t {
    File,       ///< <ES-DE home>/settings/es_settings.xml
    LegacyFile, ///< <ES-DE home>/es_settings.xml, the place of older releases: ES-DE moves it into settings/ and reads
                ///< it at its next start
    Defaults,   ///< neither file exists: ES-DE's defaults, as ES-DE runs until it first saves its settings
};

/// "file", "legacy-file", "defaults".
[[nodiscard]] std::string_view toString(SettingsSource source) noexcept;

/// One key of the whitelist.
struct SettingKey {
    std::string_view name; ///< as es_settings.xml writes it ("ROMDirectory")
    std::string_view type; ///< the element ES-DE writes it as: "bool" or "string"
};

/// The keys Retroverse reads: ROMDirectory, MediaDirectory, ShowHiddenFiles, ShowHiddenGames, ParseGamelistOnly,
/// LegacyGamelistFileLocation, AlternativeEmulatorPerGame, MAMENameStripExtraInfo, Theme (I11 step 1, ADR-0019 §2) and
/// UIMode. Nothing else is read.
[[nodiscard]] std::span<const SettingKey> settingKeys() noexcept;

/// The settings Retroverse uses, with ES-DE 3.4.1's defaults where the file does not set them.
struct EsdeSettings {
    SettingsSource source = SettingsSource::Defaults;
    /// The file read; for Defaults, where ES-DE keeps it (<ES-DE home>/settings/es_settings.xml).
    std::filesystem::path file;
    /// ROMDirectory as written; empty (the default): <ES-DE's home folder>\ROMs.
    std::string romDirectory;
    /// MediaDirectory as written; empty (the default): <ES-DE home>\downloaded_media.
    std::string mediaDirectory;
    bool showHiddenFiles = true;             ///< ShowHiddenFiles
    bool showHiddenGames = true;             ///< ShowHiddenGames
    bool parseGamelistOnly = false;          ///< ParseGamelistOnly
    bool legacyGamelistFileLocation = false; ///< LegacyGamelistFileLocation
    bool alternativeEmulatorPerGame = true;  ///< AlternativeEmulatorPerGame
    /// MAMENameStripExtraInfo: the names ES-DE gives unscraped arcade games from mamenames.xml lose their parenthesized
    /// and bracketed parts (mameName()).
    bool mameNameStripExtraInfo = true;
    UiMode uiMode = UiMode::Full;            ///< UIMode ("full" for any value ES-DE does not know)
    /// Theme as written: the folder name of ES-DE's active theme; empty (the default) is linear-es-de
    /// (esde::activeThemeName()).
    std::string theme;
    /// What was read differently than written ("setting-malformed": a boolean that is neither true nor false, an
    /// unknown UIMode, a value attribute missing), a key under another element type ("setting-wrong-type", which
    /// ES-DE ignores), a key set twice ("setting-repeated", the last counts), the legacy file ("settings-legacy-file")
    /// or none ("settings-missing"). Values are named only for the keys of the whitelist.
    std::vector<catalog::Issue> issues;

    bool operator==(const EsdeSettings&) const = default;
};

/// The largest es_settings.xml read (ES-DE writes about 9 KiB).
inline constexpr std::uintmax_t kMaxSettingsBytes = 4ull * 1024ull * 1024ull;

/// Reads one es_settings.xml file through rv_esde's XML loader (shared reading, ES-DE's decoding, no root element).
/// Errors: those of reading the file (NotFound, Unavailable, Conflict, OutOfRange beyond kMaxSettingsBytes, IoError)
/// and ParseError (it cannot be parsed, or holds no element): ES-DE would then run on its defaults and rewrite the
/// file, Retroverse refuses to guess the roots from it. The file is the context; no message holds a value of the file.
[[nodiscard]] Expected<EsdeSettings> readSettingsFile(const std::filesystem::path& file);

/// The settings of the ES-DE home `esdeHome`: settings/es_settings.xml, else the legacy es_settings.xml beside
/// settings/, else ES-DE's defaults (SettingsSource). Errors: InvalidArgument for a relative `esdeHome`, those of
/// readSettingsFile() for the file that exists.
[[nodiscard]] Expected<EsdeSettings> readSettings(const std::filesystem::path& esdeHome);

/// Where a root came from.
enum class RootSource : std::uint8_t {
    Override, ///< the library's override (LibraryProfile::romRoot, mediaRoot)
    Setting,  ///< ROMDirectory or MediaDirectory, expanded
    Default,  ///< the setting is empty: ES-DE's default
};

/// "override", "setting", "default".
[[nodiscard]] std::string_view toString(RootSource source) noexcept;

/// A ROM or media root as ES-DE would use it.
struct ResolvedRoot {
    /// The folder: absolute, '\' between the elements, no trailing separator (but a drive's root); nothing when it
    /// cannot be resolved (an issue says why). Whether it exists is not checked.
    std::optional<std::filesystem::path> path;
    RootSource source = RootSource::Default;
    /// The setting as es_settings.xml writes it, for Setting; empty otherwise.
    std::string configured;
    /// ES-DE's own text of the folder, which %ROMPATH% expands to (FileData::getROMDirectory: '~' and %ESPATH%
    /// replaced, a separator at the end, the separators as written); for an override, the override's text with a
    /// separator at the end. Empty when `path` is.
    std::string esdeText;
    /// "root-unresolved" (error: %ESPATH% without a known install folder, a relative path, text that is no path),
    /// "root-tilde-inside" (warning: ES-DE replaces every '~', not only a leading one), "root-network-share" (warning:
    /// ES-DE's generic paths collapse the leading "\\" of a network path, so ES-DE itself cannot read it; Retroverse
    /// keeps the share).
    std::vector<catalog::Issue> issues;

    bool operator==(const ResolvedRoot&) const = default;
};

/// The ROM root of the library `profile` describes: its override, else ROMDirectory with '~' replaced by ES-DE's home
/// folder (profile.esdeHome's parent) and %ESPATH% by profile.installFolder, else (empty) <home folder>\ROMs.
[[nodiscard]] ResolvedRoot resolveRomRoot(const EsdeSettings& settings, const catalog::LibraryProfile& profile);

/// The media root: the override, else MediaDirectory expanded as ROMDirectory is, else (empty)
/// <ES-DE home>\downloaded_media.
[[nodiscard]] ResolvedRoot resolveMediaRoot(const EsdeSettings& settings, const catalog::LibraryProfile& profile);

/// ES-DE's text of a folder (its "generic" form, Utils::FileSystem::getGenericPath): '/' between the elements,
/// repeated separators collapsed, no trailing separator. What '~' (the home folder) and %ESPATH% (the install folder)
/// expand to in ES-DE's paths. The leading "//" of a network path is kept, where ES-DE collapses it too and then
/// cannot reach the share: Retroverse reads it (as it reads a network root, "root-network-share").
[[nodiscard]] std::string esdeGenericText(const std::filesystem::path& folder);

/// ES-DE's Utils::String::replace: every `from` in `text` replaced with `to`, again and again until none is left
/// (once only when `to` holds `from`); how ES-DE expands '~', %ESPATH% and %ROMPATH% in its paths.
[[nodiscard]] std::string esdeReplace(std::string text, std::string_view from, std::string_view to);

/// A folder from ES-DE's text of it ('/' or '\' between the elements, repeated separators, a trailing separator, a
/// \\?\ prefix, which ES-DE drops): '\' between the elements, repeated separators collapsed (a leading "\\" of a
/// network path kept), "." and ".." resolved lexically (as Windows resolves them), no trailing separator but a drive
/// root's. Nothing for empty text, malformed UTF-8, or a path that is not absolute (relative, "C:x", "\x").
[[nodiscard]] std::optional<std::filesystem::path> folderFromEsdeText(std::string_view text);

} // namespace rv::esde
