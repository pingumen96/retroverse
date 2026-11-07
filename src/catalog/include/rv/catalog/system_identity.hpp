#pragma once

// A system's identity (ADR-0019): the name people know it by, its maker, year, kind of hardware, a description, its
// colours and its logo, read at runtime from the user's machine (ES-DE's themes, Retroverse's identity.json), never
// shipped. Each field says where it came from (IdentitySource), for `retroverse-cli identity show` and the run report.
// A field nothing gave is empty with IdentitySource::None: what shows it then falls back (the name to the shipped
// generic sign, the logo to none). Plain data; rv_esde builds it (rv/esde/identity.hpp).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/snapshot.hpp>

namespace rv::catalog {

/// Where a field of an identity came from, in the order they are tried (ADR-0019 §2).
enum class IdentitySource : std::uint8_t {
    None,       ///< nothing gave it
    Override,   ///< identity.json's per-system value
    Theme,      ///< a theme's metadata or logo: identity.json's chosen theme, else linear-es-de
    ActiveTheme, ///< the logo of ES-DE's active theme (es_settings.xml's Theme)
    FullName,   ///< ES-DE's <fullname> (the name only)
};

/// "none", "override", "theme", "active-theme", "full-name".
[[nodiscard]] constexpr std::string_view toString(IdentitySource source) noexcept {
    switch (source) {
    case IdentitySource::None:
        return "none";
    case IdentitySource::Override:
        return "override";
    case IdentitySource::Theme:
        return "theme";
    case IdentitySource::ActiveTheme:
        return "active-theme";
    case IdentitySource::FullName:
        return "full-name";
    }
    return "unknown";
}

/// A text field and where it came from. `origin` names the theme folder or the file it was read from (UTF-8, the
/// user's own path: never logged above debug level); empty for None and FullName.
struct IdentityText {
    std::string value;
    IdentitySource source = IdentitySource::None;
    std::string origin;

    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
    bool operator==(const IdentityText&) const = default;
};

/// A logo's file format.
enum class LogoFormat : std::uint8_t { Svg, Png, Jpeg };

/// "svg", "png", "jpeg".
[[nodiscard]] constexpr std::string_view toString(LogoFormat format) noexcept {
    switch (format) {
    case LogoFormat::Svg:
        return "svg";
    case LogoFormat::Png:
        return "png";
    case LogoFormat::Jpeg:
        return "jpeg";
    }
    return "unknown";
}

/// A logo file: absolute, existing when it was found, within its format's size limit.
struct IdentityLogo {
    std::filesystem::path file;
    LogoFormat format = LogoFormat::Svg;
    IdentitySource source = IdentitySource::None;
    /// The theme folder's name, or "identity.json".
    std::string origin;

    [[nodiscard]] bool empty() const noexcept { return file.empty(); }
    bool operator==(const IdentityLogo&) const = default;
};

/// The identity of one system.
struct SystemIdentity {
    SystemName system;
    /// es_systems.xml's <theme> the theme files are keyed by (the name when it has none).
    std::string themeId;
    IdentityText name;         ///< "Super Nintendo" (a theme's systemName), else ES-DE's <fullname>
    IdentityText manufacturer; ///< systemManufacturer
    IdentityText releaseYear;  ///< systemReleaseYear, four digits
    IdentityText releaseDate;  ///< systemReleaseDate, as written ("1992-04-11")
    IdentityText hardwareType; ///< systemHardwareType ("Console")
    /// systemDescription, in Italian when the theme has it (`descriptionLanguage` "it_IT"), else as the theme writes it
    /// ("" for its default language).
    IdentityText description;
    std::string descriptionLanguage;
    /// systemColor then systemColorPalette1 to 4, those that are valid, as "#RRGGBB" (uppercase), in that order.
    std::vector<std::string> colors;
    IdentitySource colorsSource = IdentitySource::None;
    IdentityLogo logo;
    /// What was left out on the way: "theme-id-invalid", "theme-metadata-unreadable", "theme-value-invalid",
    /// "theme-color-invalid", "logo-too-large", "logo-unreadable" (warnings).
    std::vector<Issue> issues;

    bool operator==(const SystemIdentity&) const = default;
};

} // namespace rv::catalog
