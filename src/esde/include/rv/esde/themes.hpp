#pragma once

// ES-DE's themes as a source of a system's identity (ADR-0019 §2): where theme folders are, the metadata a theme keeps
// per system (linear-es-de's system/metadata/<theme id>.xml: systemName, systemManufacturer, systemReleaseYear,
// systemReleaseDate, systemHardwareType, systemDescription with its translations, systemColor and
// systemColorPalette1 to 4) and the logo files of the layouts the user's themes use. Read-only, with full sharing,
// through rv_esde's XML loader; every limit below bounds what a hostile or broken theme can make Retroverse read. A
// system's theme id comes from es_systems.xml and is checked before it names a file (isThemeId()).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog/system_identity.hpp>
#include <rv/core/expected.hpp>

namespace rv::esde {

/// The theme ES-DE 3.4.1 runs when es_settings.xml names none, and the first one Retroverse reads metadata from.
inline constexpr std::string_view kDefaultThemeName = "linear-es-de";

/// The largest metadata file read (linear-es-de's are 6 to 40 KiB, with 20 translations).
inline constexpr std::uintmax_t kMaxThemeMetadataBytes = 256ull * 1024ull;
/// The largest SVG logo taken (linear-es-de's are 2 to 300 KiB).
inline constexpr std::uintmax_t kMaxSvgLogoBytes = 1024ull * 1024ull;
/// The largest PNG or JPEG logo taken.
inline constexpr std::uintmax_t kMaxRasterLogoBytes = 8ull * 1024ull * 1024ull;
/// A name, maker, year, date or kind of hardware longer than this (in bytes) is left out.
inline constexpr std::size_t kMaxThemeValueBytes = 256;
/// A description longer than this (in bytes) is cut at the last whole sentence or word before it.
inline constexpr std::size_t kMaxThemeDescriptionBytes = 4096;

/// A theme id as Retroverse accepts it to name a file: 1 to 64 characters of [A-Za-z0-9._-], not "." or "..", not
/// starting with a dot (es_systems.xml writes "snes", "megadrive", "snes-msu1", "3do").
[[nodiscard]] bool isThemeId(std::string_view id) noexcept;

/// The folders ES-DE finds themes in, in the order Retroverse searches them: <ES-DE home>/themes, then
/// <install folder>/themes (where ES-DE's bundled themes are). Folders the profile does not know are left out; whether
/// they exist is not checked.
[[nodiscard]] std::vector<std::filesystem::path> themeFolders(const catalog::LibraryProfile& profile);

/// The folder of the theme `name` in the first of `roots` that holds one (a directory named `name`); nothing when no
/// root does or `name` is no theme id.
[[nodiscard]] std::optional<std::filesystem::path> findTheme(std::span<const std::filesystem::path> roots,
                                                             std::string_view name);

/// What a theme's metadata file says about a system: each value trimmed, empty when the file does not set it or it was
/// left out (an issue says why).
struct ThemeSystemMetadata {
    std::string name;
    std::string manufacturer;
    std::string releaseYear;
    std::string releaseDate;
    std::string hardwareType;
    std::string description;
    /// The <language name> the description came from ("it_IT"), empty for the file's default variables.
    std::string descriptionLanguage;
    /// systemColor, systemColorPalette1 to 4: those that are six hexadecimal digits (with or without '#'), as
    /// "#RRGGBB" in uppercase, in that order.
    std::vector<std::string> colors;
    /// "theme-value-invalid" (a value that is too long or holds control characters), "theme-color-invalid"
    /// (warnings).
    std::vector<catalog::Issue> issues;

    bool operator==(const ThemeSystemMetadata&) const = default;
};

/// system/metadata/<id>.xml of `themeFolder`, when it is a file; nothing otherwise or when `id` is no theme id.
[[nodiscard]] std::optional<std::filesystem::path> themeMetadataFile(const std::filesystem::path& themeFolder,
                                                                     std::string_view id);

/// Reads a metadata file: a <theme> element whose <variables> hold the values, and whose <language name="...">
/// elements hold translated <variables>. The description is taken from the language `language` when the file has one,
/// else from the default variables; the other values from the default variables (a language's own value of them, as
/// linear-es-de's translated systemHardwareType, is not taken). Errors: those of reading (NotFound, Unavailable,
/// Conflict, OutOfRange past kMaxThemeMetadataBytes, IoError) and ParseError (not XML, or no <theme> element).
[[nodiscard]] Expected<ThemeSystemMetadata> readThemeMetadata(const std::filesystem::path& file,
                                                              std::string_view language = "it_IT");

/// The logo of the system `id` in `themeFolder`, in the first layout and format that holds one:
/// system/logos/<id>, _inc/systems/logos/<id>, <id>/images/logo; each as .svg, then .png, then .jpg. A file past its
/// format's limit (kMaxSvgLogoBytes, kMaxRasterLogoBytes) is passed over with a "logo-too-large" issue in `issues`
/// (when given) and the search goes on. Nothing when none is found or `id` is no theme id. The result's source and
/// origin are the caller's to set.
[[nodiscard]] std::optional<catalog::IdentityLogo> findThemeLogo(const std::filesystem::path& themeFolder,
                                                                 std::string_view id,
                                                                 std::vector<catalog::Issue>* issues = nullptr);

/// The format a logo file's extension names (.svg, .png, .jpg, .jpeg, in any case); nothing for any other.
[[nodiscard]] std::optional<catalog::LogoFormat> logoFormatOf(const std::filesystem::path& file);

} // namespace rv::esde
