#include <rv/esde/themes.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "xml.hpp"

namespace rv::esde {
namespace {

namespace fs = std::filesystem;

catalog::Issue warning(std::string code, std::string file, std::string reason) {
    return catalog::Issue{catalog::IssueSeverity::Warning, std::move(code), {}, std::move(file), std::move(reason)};
}

bool isDirectory(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}

/// The size of a regular file; nothing when it is no regular file or cannot be read.
std::optional<std::uintmax_t> regularFileSize(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        return std::nullopt;
    }
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    return size;
}

/// A C0 or C1 control character, or DEL, in UTF-8 text (tab, line feed and carriage return are whitespace, turned into
/// spaces before this is asked).
bool holdsControl(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x20 || byte == 0x7F) {
            return true;
        }
        // U+0080 to U+009F are C2 80 to C2 9F.
        if (byte == 0xC2 && i + 1 < text.size()) {
            const auto next = static_cast<unsigned char>(text[i + 1]);
            if (next >= 0x80 && next <= 0x9F) {
                return true;
            }
        }
    }
    return false;
}

/// Whitespace (spaces, tabs, line ends) collapsed to single spaces, and trimmed.
std::string collapseWhitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !out.empty();
            continue;
        }
        if (space) {
            out += ' ';
            space = false;
        }
        out += c;
    }
    return out;
}

/// The length of the longest prefix of `text` within `limit` bytes that ends on a whole UTF-8 sequence.
std::size_t utf8Prefix(std::string_view text, std::size_t limit) noexcept {
    if (text.size() <= limit) {
        return text.size();
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return end;
}

/// A description cut to kMaxThemeDescriptionBytes: at the last sentence's end before the limit, else the last space,
/// else the limit itself (on a whole UTF-8 sequence).
std::string cutDescription(std::string text) {
    if (text.size() <= kMaxThemeDescriptionBytes) {
        return text;
    }
    const std::size_t limit = utf8Prefix(text, kMaxThemeDescriptionBytes);
    const std::string_view head(text.data(), limit);
    std::size_t cut = head.rfind(". ");
    if (cut != std::string_view::npos && cut > 0) {
        cut += 1; // keep the full stop
    } else {
        cut = head.rfind(' ');
        if (cut == std::string_view::npos || cut == 0) {
            cut = limit;
        }
    }
    text.resize(cut);
    return std::string(core::trim(text));
}

/// "#RRGGBB" in uppercase from six hexadecimal digits with or without a leading '#'; nothing for anything else.
std::optional<std::string> normalizedColor(std::string_view text) {
    if (!text.empty() && text.front() == '#') {
        text.remove_prefix(1);
    }
    if (text.size() != 6 ||
        !std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
        return std::nullopt;
    }
    std::string color = "#";
    for (const char c : text) {
        color += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return color;
}

/// The text of `variables`'s child `name`, whitespace collapsed; empty when it has none.
std::string variable(const pugi::xml_node variables, const char* name) {
    if (!variables) {
        return {};
    }
    return collapseWhitespace(variables.child(name).text().as_string());
}

/// A value checked: kept when it is short enough and holds no control character, else left out with an issue.
std::string checkedValue(std::string value, std::string_view name, std::size_t limit, ThemeSystemMetadata& metadata,
                         const std::string& file) {
    if (value.empty()) {
        return value;
    }
    if (value.size() > limit || holdsControl(value) || !core::isValidUtf8(value)) {
        metadata.issues.push_back(warning("theme-value-invalid", file,
                                          fmt::format("{} is longer than {} bytes or holds control characters: it is "
                                                      "left out",
                                                      name, limit)));
        return {};
    }
    return value;
}

constexpr std::array<const char*, 5> kColorVariables{"systemColor", "systemColorPalette1", "systemColorPalette2",
                                                     "systemColorPalette3", "systemColorPalette4"};

/// The logo layouts, in the order they are searched; "{}" is the theme id.
constexpr std::array<std::string_view, 3> kLogoLayouts{"system/logos/{}", "_inc/systems/logos/{}", "{}/images/logo"};
constexpr std::array<std::string_view, 3> kLogoExtensions{".svg", ".png", ".jpg"};

} // namespace

bool isThemeId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64 || id.front() == '.') {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return std::isalnum(byte) != 0 || c == '.' || c == '_' || c == '-';
    });
}

std::vector<fs::path> themeFolders(const catalog::LibraryProfile& profile) {
    std::vector<fs::path> folders;
    if (!profile.esdeHome.empty()) {
        folders.push_back(profile.esdeHome / "themes");
    }
    if (profile.installFolder && !profile.installFolder->empty()) {
        folders.push_back(*profile.installFolder / "themes");
    }
    return folders;
}

std::optional<fs::path> findTheme(std::span<const fs::path> roots, std::string_view name) {
    if (!isThemeId(name)) {
        return std::nullopt;
    }
    const fs::path relative = core::pathFromUtf8(name);
    for (const fs::path& root : roots) {
        fs::path folder = root / relative;
        if (isDirectory(folder)) {
            return folder;
        }
    }
    return std::nullopt;
}

std::optional<fs::path> themeMetadataFile(const fs::path& themeFolder, std::string_view id) {
    if (!isThemeId(id)) {
        return std::nullopt;
    }
    fs::path file = themeFolder / "system" / "metadata" / core::pathFromUtf8(fmt::format("{}.xml", id));
    if (!regularFileSize(file)) {
        return std::nullopt;
    }
    return file;
}

Expected<ThemeSystemMetadata> readThemeMetadata(const fs::path& file, std::string_view language) {
    auto document = detail::loadXmlFile(file, kMaxThemeMetadataBytes);
    if (!document) {
        return propagate(document.error());
    }
    const std::string source = core::pathToUtf8Lossy(file);
    const pugi::xml_node theme = document->topLevel("theme");
    if (!theme) {
        return makeError(ErrorCode::ParseError, "the file holds no <theme> element", source);
    }
    const pugi::xml_node variables = theme.child("variables");

    ThemeSystemMetadata metadata;
    metadata.name =
        checkedValue(variable(variables, "systemName"), "systemName", kMaxThemeValueBytes, metadata, source);
    metadata.manufacturer = checkedValue(variable(variables, "systemManufacturer"), "systemManufacturer",
                                         kMaxThemeValueBytes, metadata, source);
    std::string year = checkedValue(variable(variables, "systemReleaseYear"), "systemReleaseYear", kMaxThemeValueBytes,
                                    metadata, source);
    if (!year.empty() &&
        (year.size() != 4 ||
         !std::all_of(year.begin(), year.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))) {
        metadata.issues.push_back(
            warning("theme-value-invalid", source, "systemReleaseYear is not four digits: it is left out"));
        year.clear();
    }
    metadata.releaseYear = std::move(year);
    metadata.releaseDate = checkedValue(variable(variables, "systemReleaseDate"), "systemReleaseDate",
                                        kMaxThemeValueBytes, metadata, source);
    metadata.hardwareType = checkedValue(variable(variables, "systemHardwareType"), "systemHardwareType",
                                         kMaxThemeValueBytes, metadata, source);

    // The description: the language's, else the default one.
    std::string description;
    if (!language.empty()) {
        for (const pugi::xml_node translated : theme.children("language")) {
            if (std::string_view(translated.attribute("name").as_string()) != language) {
                continue;
            }
            description = variable(translated.child("variables"), "systemDescription");
            if (!description.empty()) {
                metadata.descriptionLanguage = std::string(language);
            }
            break;
        }
    }
    if (description.empty()) {
        description = variable(variables, "systemDescription");
    }
    description = cutDescription(std::move(description));
    metadata.description =
        checkedValue(std::move(description), "systemDescription", kMaxThemeDescriptionBytes, metadata, source);
    if (metadata.description.empty()) {
        metadata.descriptionLanguage.clear();
    }

    for (const char* name : kColorVariables) {
        const std::string value = variable(variables, name);
        if (value.empty()) {
            continue;
        }
        auto color = normalizedColor(value);
        if (!color) {
            metadata.issues.push_back(
                warning("theme-color-invalid", source,
                        fmt::format("{} is not six hexadecimal digits: it is left out", name)));
            continue;
        }
        metadata.colors.push_back(std::move(*color));
    }
    return metadata;
}

std::optional<catalog::LogoFormat> logoFormatOf(const fs::path& file) {
    const std::string extension = core::toLowerAscii(core::pathToUtf8Lossy(file.extension()));
    if (extension == ".svg") {
        return catalog::LogoFormat::Svg;
    }
    if (extension == ".png") {
        return catalog::LogoFormat::Png;
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return catalog::LogoFormat::Jpeg;
    }
    return std::nullopt;
}

std::optional<catalog::IdentityLogo> findThemeLogo(const fs::path& themeFolder, std::string_view id,
                                                   std::vector<catalog::Issue>* issues) {
    if (!isThemeId(id)) {
        return std::nullopt;
    }
    for (const std::string_view layout : kLogoLayouts) {
        const std::string stem = fmt::format(fmt::runtime(layout), id);
        for (const std::string_view extension : kLogoExtensions) {
            fs::path file = themeFolder / core::pathFromUtf8(fmt::format("{}{}", stem, extension));
            file.make_preferred();
            const auto size = regularFileSize(file);
            if (!size) {
                continue;
            }
            const catalog::LogoFormat format = *logoFormatOf(file);
            const std::uintmax_t limit = format == catalog::LogoFormat::Svg ? kMaxSvgLogoBytes : kMaxRasterLogoBytes;
            if (*size > limit) {
                if (issues != nullptr) {
                    issues->push_back(warning("logo-too-large", core::pathToUtf8Lossy(file),
                                              fmt::format("the logo holds {} bytes, more than the {} taken: it is "
                                                          "passed over",
                                                          *size, limit)));
                }
                continue;
            }
            catalog::IdentityLogo logo;
            logo.file = std::move(file);
            logo.format = format;
            return logo;
        }
    }
    return std::nullopt;
}

} // namespace rv::esde
