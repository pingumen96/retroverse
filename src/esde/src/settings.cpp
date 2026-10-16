#include <rv/esde/settings.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

#include <fmt/format.h>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "xml.hpp"

namespace rv::esde {
namespace {

/// The whitelist, by name.
constexpr std::array<SettingKey, 8> kSettingKeys{{
    {"AlternativeEmulatorPerGame", "bool"},
    {"LegacyGamelistFileLocation", "bool"},
    {"MediaDirectory", "string"},
    {"ParseGamelistOnly", "bool"},
    {"ROMDirectory", "string"},
    {"ShowHiddenFiles", "bool"},
    {"ShowHiddenGames", "bool"},
    {"UIMode", "string"},
}};

/// The element types of es_settings.xml (Settings::loadFile).
constexpr std::array<std::string_view, 4> kSettingTypes{"bool", "int", "float", "string"};

catalog::Issue issueOf(catalog::IssueSeverity severity, std::string code, std::string file, std::string reason) {
    return catalog::Issue{severity, std::move(code), {}, std::move(file), std::move(reason)};
}

/// pugixml's xml_attribute::as_bool, which ES-DE reads booleans with: true when the value starts with 1, t, T, y or Y.
bool readsAsTrue(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    const char first = value.front();
    return first == '1' || first == 't' || first == 'T' || first == 'y' || first == 'Y';
}

/// Stores the value of the whitelisted key `key` read from `node` (an element of its own type).
void apply(EsdeSettings& settings, std::string_view key, const pugi::xml_node node, const std::string& file) {
    const pugi::xml_attribute attribute = node.attribute("value");
    const std::string_view value = attribute ? std::string_view(attribute.value()) : std::string_view();
    const auto malformed = [&](std::string reason) {
        settings.issues.push_back(
            issueOf(catalog::IssueSeverity::Warning, "setting-malformed", file, fmt::format("{} {}", key, reason)));
    };
    if (key == "ROMDirectory" || key == "MediaDirectory") {
        if (!attribute) {
            malformed("has no value attribute: ES-DE reads it as empty, its default");
        }
        (key == "ROMDirectory" ? settings.romDirectory : settings.mediaDirectory) = std::string(value);
        return;
    }
    if (key == "UIMode") {
        if (value == "full") {
            settings.uiMode = UiMode::Full;
        } else if (value == "kiosk") {
            settings.uiMode = UiMode::Kiosk;
        } else if (value == "kid") {
            settings.uiMode = UiMode::Kid;
        } else {
            settings.uiMode = UiMode::Full;
            malformed(attribute ? fmt::format("is \"{}\", which ES-DE does not know: it runs in full mode", value)
                                : std::string("has no value attribute: ES-DE runs in full mode"));
        }
        return;
    }
    // The booleans.
    const bool parsed = readsAsTrue(value);
    if (!attribute) {
        malformed("has no value attribute: ES-DE reads it as false");
    } else if (value != "true" && value != "false") {
        malformed(fmt::format("is \"{}\": ES-DE reads it as {}", value, parsed ? "true" : "false"));
    }
    if (key == "ShowHiddenFiles") {
        settings.showHiddenFiles = parsed;
    } else if (key == "ShowHiddenGames") {
        settings.showHiddenGames = parsed;
    } else if (key == "ParseGamelistOnly") {
        settings.parseGamelistOnly = parsed;
    } else if (key == "LegacyGamelistFileLocation") {
        settings.legacyGamelistFileLocation = parsed;
    } else if (key == "AlternativeEmulatorPerGame") {
        settings.alternativeEmulatorPerGame = parsed;
    }
}

/// What ES-DE's text of a folder ends with, as getROMDirectory() and getMediaDirectory() return it.
std::string withTrailingSeparator(std::string text, char separator) {
    if (!text.empty() && text.back() != separator) {
        text += separator;
    }
    return text;
}

/// The root an override names (library.json holds absolute folders).
ResolvedRoot overrideRoot(const std::filesystem::path& folder, std::string_view key) {
    ResolvedRoot root;
    root.source = RootSource::Override;
    const std::string text = core::pathToUtf8Lossy(folder);
    root.path = folderFromEsdeText(text);
    if (!root.path) {
        root.issues.push_back(issueOf(catalog::IssueSeverity::Error, "root-unresolved", {},
                                      fmt::format("the library's {} \"{}\" is no absolute folder", key, text)));
        return root;
    }
    root.esdeText = core::pathToUtf8Lossy(*root.path);
    if (!root.esdeText.ends_with('\\')) {
        root.esdeText += '\\';
    }
    return root;
}

/// Resolves the text of a root once ES-DE's replacements are known: the issues of an unresolved text, a '~' past the
/// first character, a network share.
void finishRoot(ResolvedRoot& root, std::string text, std::string_view setting, const std::string& file) {
    const auto issue = [&](catalog::IssueSeverity severity, std::string_view code, std::string reason) {
        root.issues.push_back(issueOf(severity, std::string(code), file, std::move(reason)));
    };
    if (root.source == RootSource::Setting && root.configured.find('~', 1) != std::string::npos) {
        issue(catalog::IssueSeverity::Warning, "root-tilde-inside",
              fmt::format("{} \"{}\" holds a '~' after its start: ES-DE replaces every '~' with its home folder",
                          setting, root.configured));
    }
    root.path = folderFromEsdeText(text);
    if (!root.path) {
        issue(catalog::IssueSeverity::Error, "root-unresolved",
              fmt::format("{} \"{}\" is no absolute folder (ES-DE resolves a relative one against its own working "
                          "folder, which Retroverse cannot know): set the root in library.json",
                          setting, root.configured));
        return;
    }
    root.esdeText = std::move(text);
    if (root.source == RootSource::Setting && core::pathToUtf8Lossy(*root.path).starts_with("\\\\")) {
        issue(catalog::IssueSeverity::Warning, "root-network-share",
              fmt::format("{} names a network share: ES-DE 3.4.1 collapses the leading \"\\\\\" of its paths and "
                          "cannot read it; Retroverse reads the share",
                          setting));
    }
}

/// Replaces %ESPATH% with the install folder's text; false (with the issue) when the install folder is not known.
bool expandEsPath(std::string& text, ResolvedRoot& root, std::string_view setting,
                  const catalog::LibraryProfile& profile, const std::string& file) {
    if (text.find("%ESPATH%") == std::string::npos) {
        return true;
    }
    if (!profile.installFolder) {
        root.issues.push_back(issueOf(catalog::IssueSeverity::Error, "root-unresolved", file,
                                      fmt::format("{} \"{}\" uses %ESPATH%, ES-DE's install folder, which is not known",
                                                  setting, root.configured)));
        return false;
    }
    text = esdeReplace(std::move(text), "%ESPATH%", esdeGenericText(*profile.installFolder));
    return true;
}

} // namespace

std::string_view toString(UiMode mode) noexcept {
    switch (mode) {
    case UiMode::Full:
        return "full";
    case UiMode::Kiosk:
        return "kiosk";
    case UiMode::Kid:
        return "kid";
    }
    return "full";
}

std::string_view toString(SettingsSource source) noexcept {
    switch (source) {
    case SettingsSource::File:
        return "file";
    case SettingsSource::LegacyFile:
        return "legacy-file";
    case SettingsSource::Defaults:
        return "defaults";
    }
    return "defaults";
}

std::string_view toString(RootSource source) noexcept {
    switch (source) {
    case RootSource::Override:
        return "override";
    case RootSource::Setting:
        return "setting";
    case RootSource::Default:
        return "default";
    }
    return "default";
}

std::span<const SettingKey> settingKeys() noexcept {
    return kSettingKeys;
}

Expected<EsdeSettings> readSettingsFile(const std::filesystem::path& file) {
    auto document = detail::loadXmlFile(file, kMaxSettingsBytes);
    if (!document) {
        return propagate(std::move(document.error()));
    }
    EsdeSettings settings;
    settings.source = SettingsSource::File;
    settings.file = file;
    const std::string fileText = core::pathToUtf8Lossy(file);
    std::array<int, kSettingKeys.size()> reads{};
    // ES-DE reads the top-level elements of each type in document order: a later one overrides an earlier one.
    for (const pugi::xml_node node : document->document().children()) {
        if (node.type() != pugi::node_element) {
            continue;
        }
        const std::string_view type = node.name();
        if (std::find(kSettingTypes.begin(), kSettingTypes.end(), type) == kSettingTypes.end()) {
            continue;
        }
        // Only the name is looked at, and only the value of a key of the whitelist is read.
        const std::string_view name = node.attribute("name").value();
        const auto key = std::find_if(kSettingKeys.begin(), kSettingKeys.end(),
                                      [&](const SettingKey& candidate) { return candidate.name == name; });
        if (key == kSettingKeys.end()) {
            continue;
        }
        if (key->type != type) {
            settings.issues.push_back(issueOf(
                catalog::IssueSeverity::Warning, "setting-wrong-type", fileText,
                fmt::format("{} is written as a <{}> element: ES-DE reads that setting from <{}> elements and ignores "
                            "this one",
                            name, type, key->type)));
            continue;
        }
        const auto index = static_cast<std::size_t>(key - kSettingKeys.begin());
        if (++reads[index] == 2) {
            settings.issues.push_back(issueOf(catalog::IssueSeverity::Info, "setting-repeated", fileText,
                                              fmt::format("{} is set more than once: the last one counts", name)));
        }
        apply(settings, key->name, node, fileText);
    }
    return settings;
}

Expected<EsdeSettings> readSettings(const std::filesystem::path& esdeHome) {
    if (!esdeHome.is_absolute()) {
        return makeError(ErrorCode::InvalidArgument, "the ES-DE home must be an absolute folder",
                         core::pathToUtf8Lossy(esdeHome));
    }
    const std::filesystem::path file = esdeHome / "settings" / "es_settings.xml";
    auto read = readSettingsFile(file);
    if (read || read.error().code != ErrorCode::NotFound) {
        return read;
    }
    // Older releases kept the file in the ES-DE home itself; ES-DE moves it at its next start and reads it.
    const std::filesystem::path legacy = esdeHome / "es_settings.xml";
    auto legacyRead = readSettingsFile(legacy);
    if (legacyRead) {
        legacyRead->source = SettingsSource::LegacyFile;
        legacyRead->issues.insert(
            legacyRead->issues.begin(),
            issueOf(catalog::IssueSeverity::Info, "settings-legacy-file", core::pathToUtf8Lossy(legacy),
                    "es_settings.xml is in the ES-DE home, where older releases kept it: ES-DE moves it into "
                    "settings/ at its next start"));
        return legacyRead;
    }
    if (legacyRead.error().code != ErrorCode::NotFound) {
        return legacyRead;
    }
    EsdeSettings defaults;
    defaults.source = SettingsSource::Defaults;
    defaults.file = file;
    defaults.issues.push_back(issueOf(catalog::IssueSeverity::Info, "settings-missing", core::pathToUtf8Lossy(file),
                                      "es_settings.xml does not exist: ES-DE runs on its defaults"));
    return defaults;
}

std::string esdeReplace(std::string text, std::string_view from, std::string_view to) {
    if (from.empty() || from == to) {
        return text;
    }
    while (text.find(from) != std::string::npos) {
        std::string replaced;
        std::size_t last = 0;
        std::size_t found = 0;
        while ((found = text.find(from, last)) != std::string::npos) {
            replaced.append(text, last, found - last).append(to);
            last = found + from.size();
        }
        replaced.append(text, last);
        text = std::move(replaced);
        if (to.find(from) != std::string_view::npos) {
            break;
        }
    }
    return text;
}

std::string esdeGenericText(const std::filesystem::path& folder) {
    std::string text = core::pathToUtf8Lossy(folder);
    if (text.starts_with("\\\\?\\")) {
        text.erase(0, 4);
    }
    std::replace(text.begin(), text.end(), '\\', '/');
    std::string generic;
    generic.reserve(text.size());
    for (const char c : text) {
        if (c == '/' && !generic.empty() && generic.back() == '/') {
            continue;
        }
        generic += c;
    }
    while (generic.size() > 1 && generic.back() == '/') {
        generic.pop_back();
    }
    return generic;
}

std::optional<std::filesystem::path> folderFromEsdeText(std::string_view text) {
    if (text.empty() || !core::isValidUtf8(text)) {
        return std::nullopt;
    }
    std::string slashed(text);
    std::replace(slashed.begin(), slashed.end(), '/', '\\');
    if (slashed.starts_with("\\\\?\\")) {
        slashed.erase(0, 4); // ES-DE drops the extended-length prefix (getGenericPath)
    }
    std::string folder;
    folder.reserve(slashed.size());
    std::size_t at = 0;
    if (slashed.starts_with("\\\\")) {
        folder = "\\\\"; // a network path keeps its leading pair
        at = 2;
    }
    for (; at < slashed.size(); ++at) {
        if (slashed[at] == '\\' && !folder.empty() && folder.back() == '\\') {
            continue;
        }
        folder += slashed[at];
    }
    auto path = core::tryPathFromUtf8(folder);
    if (!path || !path->is_absolute()) {
        return std::nullopt;
    }
    // "." and ".." resolved as Windows resolves them (lexically), then no trailing separator but a drive root's.
    std::wstring normal = path->lexically_normal().native();
    const auto isRoot = [&] { return normal.size() <= 3 && normal.size() >= 2 && normal[1] == L':'; };
    while (normal.size() > 2 && normal.back() == L'\\' && !isRoot()) {
        normal.pop_back();
    }
    return std::filesystem::path(std::move(normal));
}

ResolvedRoot resolveRomRoot(const EsdeSettings& settings, const catalog::LibraryProfile& profile) {
    if (profile.romRoot) {
        return overrideRoot(*profile.romRoot, "romRoot");
    }
    ResolvedRoot root;
    const std::string file = core::pathToUtf8Lossy(settings.file);
    const std::string homeText = esdeGenericText(profile.esdeHome.parent_path());
    std::string text;
    if (settings.romDirectory.empty()) {
        // FileData::getROMDirectory(): the home folder's ROMs.
        root.source = RootSource::Default;
        text = homeText + "/ROMs/";
    } else {
        root.source = RootSource::Setting;
        root.configured = settings.romDirectory;
        text = withTrailingSeparator(esdeReplace(settings.romDirectory, "~", homeText), '\\');
    }
    if (!expandEsPath(text, root, "ROMDirectory", profile, file)) {
        return root;
    }
    finishRoot(root, std::move(text), "ROMDirectory", file);
    return root;
}

ResolvedRoot resolveMediaRoot(const EsdeSettings& settings, const catalog::LibraryProfile& profile) {
    if (profile.mediaRoot) {
        return overrideRoot(*profile.mediaRoot, "mediaRoot");
    }
    ResolvedRoot root;
    const std::string file = core::pathToUtf8Lossy(settings.file);
    std::string text;
    if (settings.mediaDirectory.empty()) {
        // FileData::getMediaDirectory(): the ES-DE home's downloaded_media.
        root.source = RootSource::Default;
        text = esdeGenericText(profile.esdeHome) + "/downloaded_media/";
    } else {
        root.source = RootSource::Setting;
        root.configured = settings.mediaDirectory;
        text = esdeReplace(settings.mediaDirectory, "~", esdeGenericText(profile.esdeHome.parent_path()));
        if (!expandEsPath(text, root, "MediaDirectory", profile, file)) {
            return root;
        }
        text = withTrailingSeparator(std::move(text), '/');
    }
    finishRoot(root, std::move(text), "MediaDirectory", file);
    return root;
}

} // namespace rv::esde
