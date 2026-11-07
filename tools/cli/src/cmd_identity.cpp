// retroverse-cli identity (I11 step 1, ADR-0019 §2): each system's identity as Retroverse reads it at runtime: the
// name people know it by, its maker, year, kind of hardware, description, colours and logo, each with where it came
// from (identity.json, ES-DE's themes, ES-DE's full name). The systems are the catalog's (an import makes them); the
// themes are found from library.json's ES-DE home and install folder, and es_settings.xml's Theme. It only reads.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/system.hpp>
#include <rv/catalog/system_identity.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/arg_parser.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/identity.hpp>
#include <rv/esde/settings.hpp>

#include "catalog_text.hpp"
#include "commands.hpp"
#include "library_snapshot.hpp"

namespace rv::cli {
namespace {

constexpr std::string_view kName = "retroverse-cli identity";

constexpr std::string_view kUsage =
    "Usage: retroverse-cli [global options] identity show [--system <name>] [--json] [--output <file>]\n"
    "\n"
    "Shows each system's identity as Retroverse reads it at runtime (ADR-0019): the name, maker, year, kind of\n"
    "hardware, description, colours and logo, each with where it came from: identity.json in <user data>/config,\n"
    "a theme of ES-DE (identity.json's theme, then linear-es-de; for the logo also ES-DE's active theme), or ES-DE's\n"
    "full name. The systems are those of the catalog (an import makes them). Nothing is written.\n"
    "\n"
    "Options:\n"
    "  --system <name>  Only this system, with every field, its files and what was left out.\n"
    "  --json           Write the results as JSON.\n"
    "  --output <file>  Write the results to this file (UTF-8) instead of stdout.\n"
    "  -h, --help       Show this help and exit.\n"
    "\n"
    "Exit codes: 0 ok; 2 usage error (no library.json, an unknown system); 3 the catalog, library.json or\n"
    "identity.json could not be read.\n";

std::size_t codePoints(std::string_view utf8) noexcept {
    return static_cast<std::size_t>(
        std::count_if(utf8.begin(), utf8.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

/// "theme linear-es-de", "override", "full name", "none".
std::string sourceText(catalog::IdentitySource source, std::string_view origin) {
    switch (source) {
    case catalog::IdentitySource::Theme:
        return fmt::format("theme {}", origin);
    case catalog::IdentitySource::ActiveTheme:
        return fmt::format("active theme {}", origin);
    case catalog::IdentitySource::Override:
        return "identity.json";
    case catalog::IdentitySource::FullName:
        return "ES-DE's full name";
    case catalog::IdentitySource::None:
        return "none";
    }
    return "unknown";
}

CatalogJson textJson(const catalog::IdentityText& field) {
    CatalogJson json;
    json["value"] = field.value;
    json["source"] = catalog::toString(field.source);
    json["origin"] = field.origin;
    return json;
}

CatalogJson identityJson(const catalog::SystemIdentity& identity) {
    CatalogJson json;
    json["system"] = identity.system.str();
    json["themeId"] = identity.themeId;
    json["name"] = textJson(identity.name);
    json["manufacturer"] = textJson(identity.manufacturer);
    json["releaseYear"] = textJson(identity.releaseYear);
    json["releaseDate"] = textJson(identity.releaseDate);
    json["hardwareType"] = textJson(identity.hardwareType);
    json["description"] = textJson(identity.description);
    json["descriptionLanguage"] = identity.descriptionLanguage;
    CatalogJson colors = CatalogJson::array();
    for (const std::string& color : identity.colors) {
        colors.push_back(color);
    }
    json["colors"] = std::move(colors);
    json["colorsSource"] = catalog::toString(identity.colorsSource);
    CatalogJson logo;
    logo["file"] = identity.logo.empty() ? std::string() : core::pathToUtf8Lossy(identity.logo.file);
    logo["format"] = identity.logo.empty() ? std::string() : std::string(catalog::toString(identity.logo.format));
    logo["source"] = catalog::toString(identity.logo.source);
    logo["origin"] = identity.logo.origin;
    json["logo"] = std::move(logo);
    CatalogJson issues = CatalogJson::array();
    for (const catalog::Issue& issue : identity.issues) {
        CatalogJson item;
        item["severity"] = catalog::toString(issue.severity);
        item["code"] = issue.code;
        item["file"] = issue.file;
        item["reason"] = issue.reason;
        issues.push_back(std::move(item));
    }
    json["issues"] = std::move(issues);
    return json;
}

/// One system with every field.
std::string detailText(const catalog::SystemIdentity& identity) {
    std::string text = fmt::format("{} (theme id {})\n", identity.system.str(), identity.themeId);
    const auto field = [&](std::string_view label, const catalog::IdentityText& value) {
        text += fmt::format("  {:<13} {}{}\n", label, value.empty() ? std::string("-") : value.value,
                            value.empty() ? std::string() : fmt::format("  [{}]", sourceText(value.source,
                                                                                              value.origin)));
    };
    field("name", identity.name);
    field("manufacturer", identity.manufacturer);
    field("year", identity.releaseYear);
    field("release date", identity.releaseDate);
    field("hardware", identity.hardwareType);
    std::string colors;
    for (const std::string& color : identity.colors) {
        colors += colors.empty() ? color : " " + color;
    }
    text += fmt::format("  {:<13} {}{}\n", "colours", colors.empty() ? std::string("-") : colors,
                        colors.empty() ? std::string()
                                       : fmt::format("  [{}]", sourceText(identity.colorsSource, {})));
    if (identity.logo.empty()) {
        text += fmt::format("  {:<13} -\n", "logo");
    } else {
        text += fmt::format("  {:<13} {} {}  [{}]\n", "logo", catalog::toString(identity.logo.format),
                            core::pathToUtf8Lossy(identity.logo.file),
                            sourceText(identity.logo.source, identity.logo.origin));
    }
    if (identity.description.empty()) {
        text += fmt::format("  {:<13} -\n", "description");
    } else {
        text += fmt::format("  {:<13} [{}{}]\n    {}\n", "description",
                            sourceText(identity.description.source, identity.description.origin),
                            identity.descriptionLanguage.empty() ? std::string()
                                                                 : ", " + identity.descriptionLanguage,
                            identity.description.value);
    }
    for (const catalog::Issue& issue : identity.issues) {
        text += fmt::format("  {}: {}: {}\n", catalog::toString(issue.severity), issue.code, issue.reason);
    }
    return text;
}

} // namespace

ExitCode runIdentity(CommandContext& context) {
    if (context.arguments.empty() || context.arguments.front() == "--help" || context.arguments.front() == "-h") {
        if (context.arguments.empty()) {
            return usageError(context, "a subcommand is required: show");
        }
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    const std::string& subcommand = context.arguments.front();
    if (subcommand != "show") {
        return usageError(context, fmt::format("unknown subcommand \"{}\" (there is: show)", subcommand));
    }
    core::ArgParser parser{"retroverse-cli identity show"};
    parser.option("system", "name", "Only this system, with every field.").flag("json", "Write the results as JSON.");
    addOutputOption(parser);
    auto args = parser.parse(context.arguments.subspan(1));
    if (!args) {
        return usageError(context, args.error().toString());
    }
    if (args->has("help")) {
        context.out.write(kUsage);
        return ExitCode::Ok;
    }
    auto file = outputFileOption(*args);
    if (!file) {
        return usageError(context, file.error().toString());
    }

    ExitCode failure = ExitCode::Ok;
    const auto profile = readRecordedLibrary(context, kName, failure);
    if (!profile) {
        return failure;
    }
    auto reader = openCatalogReader(context.paths);
    if (!reader) {
        return catalogFailure(context, reader.error());
    }
    const auto summaries = reader->query->systems();
    if (!summaries) {
        return catalogFailure(context, summaries.error());
    }
    std::vector<catalog::SystemInfo> systems;
    const std::optional<std::string> only = args->value("system");
    for (const catalog_db::SystemSummary& summary : *summaries) {
        if (summary.library != profile->id) {
            continue;
        }
        if (!only || summary.system.name.str() == *only) {
            systems.push_back(summary.system);
        }
    }
    if (only && systems.empty()) {
        return usageError(context, fmt::format("the catalog has no system \"{}\"", *only));
    }

    esde::IdentityConfig config;
    const std::filesystem::path configFile = context.paths.configDir() / esde::kIdentityFileName;
    if (auto read = esde::readIdentityFile(configFile)) {
        config = std::move(*read);
    } else if (read.error().code != ErrorCode::NotFound) {
        context.err.writeLine(fmt::format("{}: {}", kName, read.error().toString()));
        return ExitCode::RuntimeFailure;
    }
    auto settings = esde::readSettings(profile->esdeHome);
    if (!settings) {
        context.err.writeLine(fmt::format("{}: ES-DE's settings cannot be read ({}): its active theme is left out",
                                          kName, settings.error().toString()));
    }
    const esde::IdentitySources sources =
        esde::identitySources(*profile, settings ? &*settings : nullptr, config);
    const std::vector<catalog::SystemIdentity> identities =
        esde::resolveSystemIdentities(systems, sources, config);

    std::string text;
    if (args->has("json")) {
        CatalogJson themes = CatalogJson::array();
        for (const auto& theme : sources.metadataThemes) {
            themes.push_back(core::pathToUtf8Lossy(theme));
        }
        CatalogJson list = CatalogJson::array();
        for (const catalog::SystemIdentity& identity : identities) {
            list.push_back(identityJson(identity));
        }
        CatalogJson json;
        json["metadataThemes"] = std::move(themes);
        json["activeTheme"] = sources.activeTheme ? core::pathToUtf8Lossy(*sources.activeTheme) : std::string();
        CatalogJson sourceIssues = CatalogJson::array();
        for (const catalog::Issue& issue : sources.issues) {
            CatalogJson item;
            item["severity"] = catalog::toString(issue.severity);
            item["code"] = issue.code;
            item["reason"] = issue.reason;
            sourceIssues.push_back(std::move(item));
        }
        json["issues"] = std::move(sourceIssues);
        json["systems"] = std::move(list);
        text = jsonText(json);
    } else if (only) {
        text = detailText(identities.front());
    } else {
        std::size_t systemWidth = 0;
        std::size_t nameWidth = 0;
        for (const catalog::SystemIdentity& identity : identities) {
            systemWidth = std::max(systemWidth, identity.system.str().size());
            nameWidth = std::max(nameWidth, codePoints(identity.name.value));
        }
        std::size_t named = 0;
        std::size_t withLogo = 0;
        std::size_t described = 0;
        for (const catalog::SystemIdentity& identity : identities) {
            named += identity.name.source == catalog::IdentitySource::Theme ||
                             identity.name.source == catalog::IdentitySource::Override
                         ? 1
                         : 0;
            withLogo += identity.logo.empty() ? 0 : 1;
            described += identity.description.empty() ? 0 : 1;
            const std::size_t padding = nameWidth - codePoints(identity.name.value);
            text += fmt::format(
                "{:<{}}  {}{}  logo {:<4}  {}{}\n", identity.system.str(), systemWidth, identity.name.value,
                std::string(padding, ' '),
                identity.logo.empty() ? std::string("-") : std::string(catalog::toString(identity.logo.format)),
                sourceText(identity.name.source, identity.name.origin),
                identity.issues.empty() ? std::string() : fmt::format("  [{} left out]", identity.issues.size()));
        }
        for (const catalog::Issue& issue : sources.issues) {
            text += fmt::format("{}: {}: {}\n", catalog::toString(issue.severity), issue.code, issue.reason);
        }
        text += fmt::format("{} system{}: {} named by a theme or identity.json, {} with a logo, {} with a "
                            "description\n",
                            identities.size(), identities.size() == 1 ? "" : "s", named, withLogo, described);
    }
    return writeResults(context, *file, text, ExitCode::Ok);
}

} // namespace rv::cli
