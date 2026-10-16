#include <rv/esde/systems.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

#include <windows.h>

#include <fmt/format.h>

#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/discovery.hpp>

#include "xml.hpp"

namespace rv::esde {
namespace {

constexpr std::string_view kBundledSystems = "systems/windows/es_systems.xml";

catalog::Issue issueOf(catalog::IssueSeverity severity, std::string code, std::string system, std::string file,
                       std::string reason) {
    return catalog::Issue{severity, std::move(code), std::move(system), std::move(file), std::move(reason)};
}

/// ES-DE's readList: the words of `text` between the delimiters (blanks and commas), in order.
std::vector<std::string> readList(std::string_view text) {
    static constexpr std::string_view kDelimiters = " \t\r\n,";
    std::vector<std::string> words;
    std::size_t start = text.find_first_not_of(kDelimiters);
    while (start != std::string_view::npos) {
        const std::size_t end = text.find_first_of(kDelimiters, start);
        words.emplace_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        start = end == std::string_view::npos ? end : text.find_first_not_of(kDelimiters, end);
    }
    return words;
}

/// Lower case as ES-DE's Utils::String::toLower gives it for the platform list (ICU); the invariant casing table,
/// ASCII when the system cannot case the text.
std::string lowerCase(std::string_view text) {
    if (auto lower = core::invariantLowerCase(text)) {
        return std::move(*lower);
    }
    return core::toLowerAscii(text);
}

/// The definitions of one file, in document order, with what ES-DE's SystemData::loadConfig skips or warns about.
struct FileContent {
    std::vector<SystemDefinition> definitions;
    bool loadExclusive = false;
};

/// ES-DE's command label rules: several commands all need a label, labels are unique.
std::vector<SystemCommand> readCommands(const pugi::xml_node system, const std::string& name,
                                        const std::string& file, std::vector<catalog::Issue>& issues) {
    std::vector<SystemCommand> commands;
    for (const pugi::xml_node entry : system.children("command")) {
        const pugi::xml_attribute label = entry.attribute("label");
        if (!label) {
            if (!commands.empty()) {
                issues.push_back(issueOf(catalog::IssueSeverity::Error, "command-label-missing", name, file,
                                         fmt::format("a command of {} has no label: ES-DE ignores it and the "
                                                     "commands after it",
                                                     name)));
                break;
            }
        } else if (!commands.empty() && commands.back().label.empty()) {
            issues.push_back(issueOf(catalog::IssueSeverity::Error, "command-label-missing", name, file,
                                     fmt::format("the first command of {} has no label: ES-DE ignores the commands "
                                                 "after it",
                                                 name)));
            break;
        }
        const std::string_view labelText = label.value();
        if (std::any_of(commands.begin(), commands.end(),
                        [&](const SystemCommand& command) { return command.label == labelText; })) {
            issues.push_back(issueOf(catalog::IssueSeverity::Error, "command-label-duplicate", name, file,
                                     fmt::format("{} has two commands labelled \"{}\": ES-DE ignores the second",
                                                 name, labelText)));
            continue;
        }
        SystemCommand command;
        command.label = std::string(labelText);
        command.text = entry.text().get();
        command.placeholder = core::toLowerAscii(command.text).find("placeholder") != std::string::npos;
        commands.push_back(std::move(command));
    }
    return commands;
}

Expected<FileContent> readSystemsFile(const std::filesystem::path& systemsFile, ConfigOrigin origin,
                                      std::vector<catalog::Issue>& issues) {
    auto document = detail::loadXmlFile(systemsFile, kMaxSystemsBytes);
    if (!document) {
        return propagate(std::move(document.error()));
    }
    const std::string file = document->source();
    FileContent content;
    content.loadExclusive = static_cast<bool>(document->topLevel("loadExclusive"));
    const pugi::xml_node systemList = document->topLevel("systemList");
    if (!systemList) {
        return makeError(ErrorCode::ParseError, "es_systems.xml has no <systemList> element (ES-DE refuses to start)",
                         file);
    }
    for (const pugi::xml_node system : systemList.children("system")) {
        std::string name = esdeReplace(system.child("name").text().get(), "\n", "");
        std::string fullName = esdeReplace(system.child("fullname").text().get(), "\n", "");
        const std::string path = system.child("path").text().get();
        std::vector<std::string> extensions = readList(system.child("extension").text().get());
        std::vector<catalog::Issue> commandIssues; // reported for the systems kept only
        std::vector<SystemCommand> commands = readCommands(system, name, file, commandIssues);
        if (name.empty()) {
            issues.push_back(issueOf(catalog::IssueSeverity::Error, "system-skipped", {}, file,
                                     "a system has no name: ES-DE skips it"));
            continue;
        }
        if (fullName.empty() || path.empty() || extensions.empty() || commands.empty()) {
            issues.push_back(issueOf(catalog::IssueSeverity::Error, "system-skipped", name, file,
                                     fmt::format("{} has no full name, path, extension or command: ES-DE skips it",
                                                 name)));
            continue;
        }
        auto systemName = catalog::SystemName::parse(name);
        if (!systemName) {
            issues.push_back(issueOf(catalog::IssueSeverity::Error, "system-name-invalid", name, file,
                                     fmt::format("the system name \"{}\" is no valid folder name: skipped", name)));
            continue;
        }
        issues.insert(issues.end(), commandIssues.begin(), commandIssues.end());
        SystemDefinition definition;
        definition.name = std::move(*systemName);
        definition.fullName = std::move(fullName);
        definition.sortName = system.child("systemsortname").text().get();
        definition.path = path;
        definition.extensions = std::move(extensions);
        definition.commands = std::move(commands);
        definition.platforms = readList(lowerCase(system.child("platform").text().get()));
        definition.theme = system.child("theme").text().as_string(name.c_str());
        definition.origin = origin;
        for (const std::string& extension : definition.extensions) {
            if (!extension.starts_with('.')) { // "." itself means files without an extension
                issues.push_back(
                    issueOf(catalog::IssueSeverity::Info, "extension-without-dot", name, file,
                            fmt::format("the extension \"{}\" of {} has no leading dot: ES-DE compares it as written, "
                                        "so it matches no file",
                                        extension, name)));
            }
        }
        if (definition.platforms.empty()) {
            issues.push_back(issueOf(catalog::IssueSeverity::Info, "platform-missing", name, file,
                                     fmt::format("{} has no platform", name)));
        }
        content.definitions.push_back(std::move(definition));
    }
    return content;
}

bool pathExists(const std::filesystem::path& path) {
    return GetFileAttributesW(core::extendedLengthPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

std::string_view toString(ConfigOrigin origin) noexcept {
    switch (origin) {
    case ConfigOrigin::Bundled:
        return "bundled";
    case ConfigOrigin::Custom:
        return "custom";
    }
    return "bundled";
}

const SystemCommand* SystemDefinition::findCommand(std::string_view label) const noexcept {
    const auto found = std::find_if(commands.begin(), commands.end(),
                                    [&](const SystemCommand& command) { return command.label == label; });
    return found == commands.end() ? nullptr : &*found;
}

const SystemDefinition* SystemsConfiguration::find(std::string_view name) const noexcept {
    const auto found = std::lower_bound(systems.begin(), systems.end(), name, [](const SystemDefinition& system,
                                                                                std::string_view wanted) {
        return std::string_view(system.name.str()) < wanted;
    });
    return found != systems.end() && found->name.str() == name ? &*found : nullptr;
}

std::size_t SystemsConfiguration::commandCount() const noexcept {
    std::size_t count = 0;
    for (const SystemDefinition& system : systems) {
        count += system.commands.size();
    }
    return count;
}

Expected<SystemsConfiguration> loadSystemsFiles(const std::optional<std::filesystem::path>& customFile,
                                                const std::filesystem::path& bundledFile) {
    SystemsConfiguration result;
    result.bundledFile = bundledFile;
    result.customFile = customFile;
    std::vector<SystemDefinition> ordered; // ES-DE's reading order: the custom file's, then the bundled file's
    if (customFile) {
        auto custom = readSystemsFile(*customFile, ConfigOrigin::Custom, result.issues);
        if (!custom) {
            return propagate(std::move(custom.error()));
        }
        result.loadExclusive = custom->loadExclusive;
        ordered = std::move(custom->definitions);
    }
    if (!result.loadExclusive) {
        auto bundled = readSystemsFile(bundledFile, ConfigOrigin::Bundled, result.issues);
        if (!bundled) {
            return propagate(std::move(bundled.error()));
        }
        if (bundled->loadExclusive) {
            result.issues.push_back(issueOf(catalog::IssueSeverity::Warning, "load-exclusive-ignored", {},
                                            core::pathToUtf8Lossy(bundledFile),
                                            "the bundled es_systems.xml holds <loadExclusive/>: ES-DE ignores it "
                                            "there"));
        }
        std::move(bundled->definitions.begin(), bundled->definitions.end(), std::back_inserter(ordered));
    }
    for (SystemDefinition& definition : ordered) {
        const auto first =
            std::find_if(result.systems.begin(), result.systems.end(),
                         [&](const SystemDefinition& kept) { return kept.name == definition.name; });
        if (first == result.systems.end()) {
            result.systems.push_back(std::move(definition));
            continue;
        }
        const std::string name = definition.name.str();
        const std::string file = core::pathToUtf8Lossy(
            definition.origin == ConfigOrigin::Custom && customFile ? *customFile : bundledFile);
        if (first->origin == ConfigOrigin::Custom && definition.origin == ConfigOrigin::Bundled) {
            result.issues.push_back(issueOf(catalog::IssueSeverity::Info, "system-replaced", name, file,
                                            fmt::format("the custom definition of {} replaces the bundled one", name)));
        } else {
            result.issues.push_back(issueOf(catalog::IssueSeverity::Warning, "system-repeated", name, file,
                                            fmt::format("{} is defined again: ES-DE uses the later definition only "
                                                        "when the earlier one finds no game",
                                                        name)));
        }
        result.fallbacks.push_back(std::move(definition));
    }
    std::sort(result.systems.begin(), result.systems.end(),
              [](const SystemDefinition& a, const SystemDefinition& b) { return a.name < b.name; });
    return result;
}

Expected<SystemsConfiguration> loadSystems(const catalog::LibraryProfile& profile) {
    if (!profile.esdeHome.is_absolute()) {
        return makeError(ErrorCode::InvalidArgument, "the ES-DE home must be an absolute folder",
                         core::pathToUtf8Lossy(profile.esdeHome));
    }
    const auto bundled = bundledResource(profile, kBundledSystems);
    if (!bundled) {
        return makeError(ErrorCode::NotFound,
                         "ES-DE's bundled es_systems.xml was found neither in the ES-DE home's resources nor in the "
                         "install folder's",
                         std::string(kBundledSystems));
    }
    std::optional<std::filesystem::path> custom = profile.esdeHome / "custom_systems" / "es_systems.xml";
    if (!pathExists(*custom)) {
        custom.reset();
    }
    return loadSystemsFiles(custom, *bundled);
}

std::optional<std::filesystem::path> systemRomFolder(const SystemDefinition& system, const ResolvedRoot& romRoot,
                                                     const catalog::LibraryProfile& profile) {
    if (system.path.find("%ROMPATH%") != std::string::npos && !romRoot.path) {
        return std::nullopt;
    }
    // SystemData::loadConfig: %ROMPATH%, then '~'; ES-DE also turns '\' into '/' and collapses "//", which
    // folderFromEsdeText() does but for a network path's leading pair. An override is no text of ES-DE's, which never
    // sees it: '~' is replaced in the system's path alone, never in the override's folder ("D:\EMULAT~1").
    const std::string home = esdeGenericText(profile.esdeHome.parent_path());
    if (romRoot.source == RootSource::Override) {
        return folderFromEsdeText(esdeReplace(esdeReplace(system.path, "~", home), "%ROMPATH%", romRoot.esdeText));
    }
    std::string text = esdeReplace(system.path, "%ROMPATH%", romRoot.esdeText);
    text = esdeReplace(std::move(text), "~", home);
    return folderFromEsdeText(text);
}

std::vector<std::filesystem::path> readOnlyFolders(const catalog::LibraryProfile& profile,
                                                   const std::optional<ResolvedRoot>& romRoot,
                                                   const std::optional<ResolvedRoot>& mediaRoot,
                                                   const SystemsConfiguration* systems) {
    std::vector<std::filesystem::path> folders;
    const auto add = [&](const std::filesystem::path& folder) {
        const bool listed = std::any_of(folders.begin(), folders.end(), [&](const std::filesystem::path& known) {
            return core::pathIsWithin(folder, known);
        });
        if (!folder.empty() && !listed) {
            folders.push_back(folder);
        }
    };
    add(profile.esdeHome);
    if (profile.installFolder) {
        add(*profile.installFolder);
    }
    for (const std::optional<ResolvedRoot>* root : {&romRoot, &mediaRoot}) {
        if (*root && (*root)->path) {
            add(*(*root)->path);
        }
    }
    if (systems != nullptr) {
        const ResolvedRoot unknown;
        for (const std::vector<SystemDefinition>* list : {&systems->systems, &systems->fallbacks}) {
            for (const SystemDefinition& system : *list) {
                if (const auto folder = systemRomFolder(system, romRoot ? *romRoot : unknown, profile)) {
                    add(*folder);
                }
            }
        }
    }
    return folders;
}

std::vector<std::filesystem::path> readOnlyFolders(const catalog::LibraryProfile& profile) {
    std::optional<ResolvedRoot> romRoot;
    std::optional<ResolvedRoot> mediaRoot;
    const auto settings = readSettings(profile.esdeHome);
    const EsdeSettings none;
    if (settings || profile.romRoot) {
        romRoot = resolveRomRoot(settings ? *settings : none, profile);
    }
    if (settings || profile.mediaRoot) {
        mediaRoot = resolveMediaRoot(settings ? *settings : none, profile);
    }
    const auto systems = loadSystems(profile);
    return readOnlyFolders(profile, romRoot, mediaRoot, systems ? &*systems : nullptr);
}

std::vector<catalog::SystemInfo> systemInfos(const SystemsConfiguration& systems, const ResolvedRoot& romRoot,
                                             const catalog::LibraryProfile& profile,
                                             std::vector<catalog::Issue>& issues) {
    std::vector<catalog::SystemInfo> infos;
    infos.reserve(systems.systems.size());
    for (const SystemDefinition& system : systems.systems) {
        catalog::SystemInfo info;
        info.name = system.name;
        info.fullName = system.fullName;
        info.platforms = system.platforms;
        info.theme = system.theme;
        info.romFolderConfigured = system.path;
        if (const auto folder = systemRomFolder(system, romRoot, profile)) {
            info.romFolder = *folder;
            const FolderCheck check = checkFolder(*folder);
            info.availability = check.availability;
            info.unavailableReason = check.reason;
        } else {
            info.availability = catalog::Availability::SourceUnavailable;
            info.unavailableReason = catalog::SourceUnavailableReason::FolderMissing;
            if (system.path.find("%ROMPATH%") == std::string::npos || romRoot.path) {
                issues.push_back(issueOf(catalog::IssueSeverity::Error, "rom-folder-unresolved", system.name.str(),
                                         {},
                                         fmt::format("the path \"{}\" of {} gives no absolute folder", system.path,
                                                     system.name.str())));
            }
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

std::filesystem::path gamelistFile(const catalog::SystemName& name,
                                   const std::optional<std::filesystem::path>& romFolder, const EsdeSettings& settings,
                                   const std::filesystem::path& esdeHome) {
    if (settings.legacyGamelistFileLocation && romFolder) {
        std::filesystem::path legacy = *romFolder / "gamelist.xml";
        if (pathExists(legacy)) {
            return legacy;
        }
    }
    auto folder = core::tryPathFromUtf8(name.str());
    return esdeHome / "gamelists" / (folder ? *folder : std::filesystem::path()) / "gamelist.xml";
}

} // namespace rv::esde
