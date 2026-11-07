#include <rv/esde/identity.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/themes.hpp>

namespace rv::esde {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

catalog::Issue issue(catalog::IssueSeverity severity, std::string code, std::string system, std::string file,
                     std::string reason) {
    return catalog::Issue{severity, std::move(code), std::move(system), std::move(file), std::move(reason)};
}

/// "#RRGGBB" in uppercase from six hexadecimal digits with or without '#'.
std::optional<std::string> colorOf(std::string_view text) {
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

/// An absolute path from a JSON string.
Expected<fs::path> absolutePath(const json& value, std::string_view key, const std::string& context) {
    if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("\"{}\" must be a non-empty string", key), context);
    }
    auto path = core::tryPathFromUtf8(value.get_ref<const std::string&>());
    if (!path) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("\"{}\" is not valid UTF-8", key), context);
    }
    if (!path->is_absolute()) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("\"{}\" must be an absolute path", key), context);
    }
    return path->lexically_normal().make_preferred();
}

Expected<IdentityOverride> overrideOf(const json& value, const std::string& system, const std::string& context) {
    if (!value.is_object()) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("\"systems\".\"{}\" must be an object", system),
                         context);
    }
    IdentityOverride result;
    for (const auto& item : value.items()) {
        const std::string& key = item.key();
        const json& field = item.value();
        const auto text = [&](std::optional<std::string>& to) -> Status {
            if (!field.is_string()) {
                return makeError(ErrorCode::InvalidArgument,
                                 fmt::format("\"systems\".\"{}\".\"{}\" must be a string", system, key), context);
            }
            to = std::string(core::trim(field.get_ref<const std::string&>()));
            return {};
        };
        Status status;
        if (key == "name") {
            status = text(result.name);
        } else if (key == "manufacturer") {
            status = text(result.manufacturer);
        } else if (key == "releaseYear") {
            status = text(result.releaseYear);
        } else if (key == "releaseDate") {
            status = text(result.releaseDate);
        } else if (key == "hardwareType") {
            status = text(result.hardwareType);
        } else if (key == "description") {
            status = text(result.description);
        } else if (key == "colors") {
            if (!field.is_array()) {
                return makeError(ErrorCode::InvalidArgument,
                                 fmt::format("\"systems\".\"{}\".\"colors\" must be an array", system), context);
            }
            std::vector<std::string> colors;
            for (const json& color : field) {
                auto parsed = color.is_string() ? colorOf(color.get_ref<const std::string&>()) : std::nullopt;
                if (!parsed) {
                    return makeError(
                        ErrorCode::InvalidArgument,
                        fmt::format("\"systems\".\"{}\".\"colors\" holds a value that is not \"#RRGGBB\"", system),
                        context);
                }
                colors.push_back(std::move(*parsed));
            }
            result.colors = std::move(colors);
        } else if (key == "logo") {
            auto path = absolutePath(field, fmt::format("systems.{}.logo", system), context);
            if (!path) {
                return propagate(path.error());
            }
            if (!logoFormatOf(*path)) {
                return makeError(
                    ErrorCode::InvalidArgument,
                    fmt::format("\"systems\".\"{}\".\"logo\" must be an .svg, .png or .jpg file", system), context);
            }
            result.logo = std::move(*path);
        } else {
            return makeError(ErrorCode::InvalidArgument,
                             fmt::format("unknown key \"{}\" in \"systems\".\"{}\"", key, system), context);
        }
        if (!status) {
            return propagate(status.error());
        }
    }
    return result;
}

std::optional<std::uintmax_t> fileSize(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        return std::nullopt;
    }
    const std::uintmax_t size = fs::file_size(path, error);
    return error ? std::nullopt : std::optional<std::uintmax_t>(size);
}

std::string folderName(const fs::path& folder) {
    return core::pathToUtf8Lossy(folder.filename());
}

/// Sets a text field from an override.
void apply(catalog::IdentityText& field, const std::optional<std::string>& value) {
    if (value && !value->empty()) {
        field = catalog::IdentityText{*value, catalog::IdentitySource::Override, std::string(kIdentityFileName)};
    }
}

} // namespace

Expected<IdentityConfig> readIdentityFile(const fs::path& file) {
    const std::string context = core::pathToUtf8Lossy(file);
    const auto text = core::readTextFile(file, kMaxIdentityFileBytes);
    if (!text) {
        return propagate(text.error());
    }
    const auto document = core::parseJson(*text, context);
    if (!document) {
        return propagate(document.error());
    }
    if (!document->is_object()) {
        return makeError(ErrorCode::ParseError, "identity.json must hold a JSON object", context);
    }
    static constexpr std::array<std::string_view, 5> kKeys{"schemaVersion", "theme", "themeRoots", "activeTheme",
                                                           "systems"};
    for (const auto& item : document->items()) {
        if (std::find(kKeys.begin(), kKeys.end(), item.key()) == kKeys.end()) {
            return makeError(ErrorCode::InvalidArgument, fmt::format("unknown key \"{}\"", item.key()), context);
        }
    }
    const auto version = document->find("schemaVersion");
    if (version == document->end() || !version->is_number_integer()) {
        return makeError(ErrorCode::InvalidArgument, "\"schemaVersion\" must be an integer", context);
    }
    if (version->get<long long>() != kIdentityFileSchemaVersion) {
        return makeError(ErrorCode::Unsupported,
                         fmt::format("schemaVersion {} is not supported (this version reads {})",
                                     version->get<long long>(), kIdentityFileSchemaVersion),
                         context);
    }
    IdentityConfig config;
    if (const auto theme = document->find("theme"); theme != document->end() && !theme->is_null()) {
        if (!theme->is_string() || !isThemeId(theme->get_ref<const std::string&>())) {
            return makeError(ErrorCode::InvalidArgument,
                             "\"theme\" must be a theme folder's name (letters, digits, '.', '_' and '-')", context);
        }
        config.theme = theme->get<std::string>();
    }
    if (const auto roots = document->find("themeRoots"); roots != document->end() && !roots->is_null()) {
        if (!roots->is_array()) {
            return makeError(ErrorCode::InvalidArgument, "\"themeRoots\" must be an array of folders", context);
        }
        for (const json& root : *roots) {
            auto folder = absolutePath(root, "themeRoots", context);
            if (!folder) {
                return propagate(folder.error());
            }
            config.themeRoots.push_back(std::move(*folder));
        }
    }
    if (const auto active = document->find("activeTheme"); active != document->end() && !active->is_null()) {
        if (!active->is_boolean()) {
            return makeError(ErrorCode::InvalidArgument, "\"activeTheme\" must be true or false", context);
        }
        config.activeTheme = active->get<bool>();
    }
    if (const auto systems = document->find("systems"); systems != document->end() && !systems->is_null()) {
        if (!systems->is_object()) {
            return makeError(ErrorCode::InvalidArgument, "\"systems\" must be an object", context);
        }
        for (const auto& item : systems->items()) {
            if (item.key().empty()) {
                return makeError(ErrorCode::InvalidArgument, "\"systems\" holds an empty system name", context);
            }
            auto parsed = overrideOf(item.value(), item.key(), context);
            if (!parsed) {
                return propagate(parsed.error());
            }
            config.systems.emplace(item.key(), std::move(*parsed));
        }
    }
    return config;
}

std::string activeThemeName(const EsdeSettings& settings) {
    return settings.theme.empty() ? std::string(kDefaultThemeName) : settings.theme;
}

IdentitySources identitySources(const catalog::LibraryProfile& profile, const EsdeSettings* settings,
                                const IdentityConfig& config) {
    IdentitySources sources;
    sources.roots = config.themeRoots;
    for (fs::path& folder : themeFolders(profile)) {
        sources.roots.push_back(std::move(folder));
    }
    const auto notFound = [&](catalog::IssueSeverity severity, std::string_view name, std::string_view what) {
        sources.issues.push_back(issue(severity, "theme-not-found", {}, {},
                                       fmt::format("the theme \"{}\" ({}) is in none of the theme folders", name,
                                                   what)));
    };
    std::vector<std::string> names;
    if (!config.theme.empty()) {
        names.push_back(config.theme);
    }
    if (std::find(names.begin(), names.end(), kDefaultThemeName) == names.end()) {
        names.emplace_back(kDefaultThemeName);
    }
    for (const std::string& name : names) {
        auto folder = findTheme(sources.roots, name);
        if (folder) {
            sources.metadataThemes.push_back(std::move(*folder));
        } else if (name == kDefaultThemeName) {
            notFound(catalog::IssueSeverity::Info, name, "ES-DE's bundled theme");
        } else {
            notFound(catalog::IssueSeverity::Warning, name, "identity.json's theme");
        }
    }
    if (config.activeTheme) {
        const std::string active = settings != nullptr ? activeThemeName(*settings) : std::string(kDefaultThemeName);
        if (std::find(names.begin(), names.end(), active) == names.end()) {
            auto folder = findTheme(sources.roots, active);
            if (folder) {
                sources.activeTheme = std::move(*folder);
            } else {
                notFound(catalog::IssueSeverity::Warning, active, "ES-DE's active theme");
            }
        }
    }
    return sources;
}

std::vector<catalog::SystemIdentity> resolveSystemIdentities(std::span<const catalog::SystemInfo> systems,
                                                             const IdentitySources& sources,
                                                             const IdentityConfig& config) {
    std::vector<catalog::SystemIdentity> identities;
    identities.reserve(systems.size());
    for (const catalog::SystemInfo& info : systems) {
        catalog::SystemIdentity identity;
        identity.system = info.name;
        const std::string& system = info.name.str();
        identity.themeId = info.theme.empty() ? system : info.theme;
        const bool validId = isThemeId(identity.themeId);
        if (!validId) {
            identity.issues.push_back(issue(catalog::IssueSeverity::Warning, "theme-id-invalid", system, {},
                                            "the system's <theme> cannot name a theme file: no theme is read for it"));
        }

        // The metadata and logo of the first metadata theme that has the system.
        if (validId) {
            for (const fs::path& theme : sources.metadataThemes) {
                const auto file = themeMetadataFile(theme, identity.themeId);
                if (!file) {
                    continue;
                }
                auto metadata = readThemeMetadata(*file);
                if (!metadata) {
                    identity.issues.push_back(issue(catalog::IssueSeverity::Warning, "theme-metadata-unreadable",
                                                    system, core::pathToUtf8Lossy(*file),
                                                    metadata.error().message));
                    continue;
                }
                const std::string origin = folderName(theme);
                const auto set = [&](catalog::IdentityText& field, std::string value) {
                    if (!value.empty()) {
                        field = catalog::IdentityText{std::move(value), catalog::IdentitySource::Theme, origin};
                    }
                };
                set(identity.name, std::move(metadata->name));
                set(identity.manufacturer, std::move(metadata->manufacturer));
                set(identity.releaseYear, std::move(metadata->releaseYear));
                set(identity.releaseDate, std::move(metadata->releaseDate));
                set(identity.hardwareType, std::move(metadata->hardwareType));
                set(identity.description, std::move(metadata->description));
                if (!identity.description.empty()) {
                    identity.descriptionLanguage = std::move(metadata->descriptionLanguage);
                }
                if (!metadata->colors.empty()) {
                    identity.colors = std::move(metadata->colors);
                    identity.colorsSource = catalog::IdentitySource::Theme;
                }
                for (catalog::Issue& found : metadata->issues) {
                    found.system = system;
                    identity.issues.push_back(std::move(found));
                }
                break;
            }
            for (const fs::path& theme : sources.metadataThemes) {
                std::vector<catalog::Issue> logoIssues;
                auto logo = findThemeLogo(theme, identity.themeId, &logoIssues);
                for (catalog::Issue& found : logoIssues) {
                    found.system = system;
                    identity.issues.push_back(std::move(found));
                }
                if (logo) {
                    logo->source = catalog::IdentitySource::Theme;
                    logo->origin = folderName(theme);
                    identity.logo = std::move(*logo);
                    break;
                }
            }
            if (identity.logo.empty() && sources.activeTheme) {
                std::vector<catalog::Issue> logoIssues;
                auto logo = findThemeLogo(*sources.activeTheme, identity.themeId, &logoIssues);
                for (catalog::Issue& found : logoIssues) {
                    found.system = system;
                    identity.issues.push_back(std::move(found));
                }
                if (logo) {
                    logo->source = catalog::IdentitySource::ActiveTheme;
                    logo->origin = folderName(*sources.activeTheme);
                    identity.logo = std::move(*logo);
                }
            }
        }

        // identity.json wins.
        if (const auto found = config.systems.find(system); found != config.systems.end()) {
            const IdentityOverride& values = found->second;
            apply(identity.name, values.name);
            apply(identity.manufacturer, values.manufacturer);
            apply(identity.releaseYear, values.releaseYear);
            apply(identity.releaseDate, values.releaseDate);
            apply(identity.hardwareType, values.hardwareType);
            if (values.description && !values.description->empty()) {
                apply(identity.description, values.description);
                identity.descriptionLanguage.clear();
            }
            if (values.colors) {
                identity.colors = *values.colors;
                identity.colorsSource = catalog::IdentitySource::Override;
            }
            if (values.logo) {
                const auto size = fileSize(*values.logo);
                const catalog::LogoFormat format = *logoFormatOf(*values.logo);
                const std::uintmax_t limit =
                    format == catalog::LogoFormat::Svg ? kMaxSvgLogoBytes : kMaxRasterLogoBytes;
                if (!size) {
                    identity.issues.push_back(issue(catalog::IssueSeverity::Warning, "logo-unreadable", system,
                                                    core::pathToUtf8Lossy(*values.logo),
                                                    "identity.json's logo is no file: the themes' logo is kept"));
                } else if (*size > limit) {
                    identity.issues.push_back(issue(
                        catalog::IssueSeverity::Warning, "logo-too-large", system,
                        core::pathToUtf8Lossy(*values.logo),
                        fmt::format("identity.json's logo holds {} bytes, more than the {} taken: the themes' logo is "
                                    "kept",
                                    *size, limit)));
                } else {
                    identity.logo = catalog::IdentityLogo{*values.logo, format, catalog::IdentitySource::Override,
                                                          std::string(kIdentityFileName)};
                }
            }
        }

        // ES-DE's full name, for the name alone.
        if (identity.name.empty() && !info.fullName.empty()) {
            identity.name = catalog::IdentityText{info.fullName, catalog::IdentitySource::FullName, {}};
        }
        identities.push_back(std::move(identity));
    }
    return identities;
}

} // namespace rv::esde
