#pragma once

// The systems' identity (ADR-0019 §2): Retroverse's <user data>/config/identity.json, and the resolution of each
// system's catalog::SystemIdentity from it, ES-DE's themes (rv/esde/themes.hpp) and ES-DE's <fullname>. Read-only:
// identity.json is the user's, as every file of <user data>/config; the themes are ES-DE's.
//
// identity.json (JSON with comments; every key optional):
//   {"schemaVersion": 1,
//    "theme": "art-book-next-es-de",           // a theme folder's name, read before linear-es-de
//    "themeRoots": ["D:\\Themes"],             // absolute folders searched for themes before ES-DE's
//    "activeTheme": true,                      // false: never take logos from ES-DE's active theme
//    "systems": {"snes": {"name": "...", "manufacturer": "...", "releaseYear": "1992", "releaseDate": "...",
//                         "hardwareType": "...", "description": "...", "colors": ["#DF5142"],
//                         "logo": "D:\\Logos\\snes.svg"}}}
// A value given for a system wins over every theme ("override"); unknown keys are errors.
//
// The order of each field (the first that gives it wins): identity.json's systems, then the metadata and logo of the
// first metadata theme that has the system (identity.json's "theme", then linear-es-de), then for the logo alone the
// active theme (es_settings.xml's Theme, linear-es-de when it names none), then for the name alone ES-DE's <fullname>.

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/library.hpp>
#include <rv/catalog/system.hpp>
#include <rv/catalog/system_identity.hpp>
#include <rv/core/expected.hpp>
#include <rv/esde/settings.hpp>

namespace rv::esde {

/// The file under <user data>/config.
inline constexpr std::string_view kIdentityFileName = "identity.json";
inline constexpr int kIdentityFileSchemaVersion = 1;
/// The largest identity.json read.
inline constexpr std::uintmax_t kMaxIdentityFileBytes = 1024ull * 1024ull;

/// identity.json's values for one system; nothing where it gives none.
struct IdentityOverride {
    std::optional<std::string> name;
    std::optional<std::string> manufacturer;
    std::optional<std::string> releaseYear;
    std::optional<std::string> releaseDate;
    std::optional<std::string> hardwareType;
    std::optional<std::string> description;
    /// "#RRGGBB" each (with or without '#' in the file), uppercase.
    std::optional<std::vector<std::string>> colors;
    /// An absolute .svg, .png or .jpg file.
    std::optional<std::filesystem::path> logo;

    bool operator==(const IdentityOverride&) const = default;
};

/// identity.json as read; a default value when the file does not exist.
struct IdentityConfig {
    /// A theme folder's name read before linear-es-de; empty for none.
    std::string theme;
    /// Absolute folders searched for themes before <ES-DE home>/themes and <install>/themes.
    std::vector<std::filesystem::path> themeRoots;
    /// Whether logos are taken from ES-DE's active theme.
    bool activeTheme = true;
    /// By system name, as es_systems.xml writes it.
    std::map<std::string, IdentityOverride, std::less<>> systems;

    bool operator==(const IdentityConfig&) const = default;
};

/// Reads identity.json. Errors: NotFound when it does not exist (the caller takes IdentityConfig{}), those of reading
/// (OutOfRange past kMaxIdentityFileBytes, IoError), ParseError (not JSON), InvalidArgument (an unknown key, a value
/// of the wrong type, a relative folder or file, a colour that is not six hexadecimal digits, a theme that is no
/// theme id) and Unsupported (another schemaVersion), with the file as the context.
[[nodiscard]] Expected<IdentityConfig> readIdentityFile(const std::filesystem::path& file);

/// The name of ES-DE's active theme: es_settings.xml's Theme, kDefaultThemeName when it names none.
[[nodiscard]] std::string activeThemeName(const EsdeSettings& settings);

/// Where the identities are read from: the theme folders found, in the order they are used.
struct IdentitySources {
    /// The folders searched for themes: identity.json's themeRoots, then themeFolders() of the library.
    std::vector<std::filesystem::path> roots;
    /// The metadata themes found, in order (identity.json's theme, then linear-es-de), each its folder; themes that
    /// were not found are left out.
    std::vector<std::filesystem::path> metadataThemes;
    /// The active theme's folder when it is found, is no metadata theme and identity.json does not turn it off.
    std::optional<std::filesystem::path> activeTheme;
    /// "theme-not-found" (warning: identity.json's theme, or the active theme, is in none of the roots; "info" for
    /// linear-es-de, which a library without an install folder cannot reach).
    std::vector<catalog::Issue> issues;

    bool operator==(const IdentitySources&) const = default;
};

/// The theme folders of a library: its profile's ES-DE home and install folder, its settings' active theme (nothing:
/// ES-DE's default), identity.json.
[[nodiscard]] IdentitySources identitySources(const catalog::LibraryProfile& profile, const EsdeSettings* settings,
                                              const IdentityConfig& config);

/// The identity of each of `systems` (in their order), from `sources` and `config` as the header describes. Reads
/// one metadata file per system at most from each metadata theme until one has it, and checks a few file names for
/// its logo. Never fails: what cannot be read is an issue of its system's identity.
[[nodiscard]] std::vector<catalog::SystemIdentity> resolveSystemIdentities(std::span<const catalog::SystemInfo> systems,
                                                                           const IdentitySources& sources,
                                                                           const IdentityConfig& config);

} // namespace rv::esde
