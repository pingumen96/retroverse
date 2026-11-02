#include <rv/esde/change_scope.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include <rv/catalog/media.hpp>
#include <rv/core/case_fold.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/media_resolver.hpp>

namespace rv::esde {

namespace {

using Elements = std::vector<std::wstring>;

/// `text` upper-cased as the file system compares names; empty when it cannot be.
[[nodiscard]] std::wstring folded(std::string_view text) {
    const auto wide = core::utf8ToWide(text);
    if (!wide) {
        return {};
    }
    auto upper = core::fileSystemUpperCase(*wide);
    return upper ? std::move(*upper) : std::wstring();
}

/// The extension of a folded name as ES-DE takes it (the last '.' on, "." without one).
[[nodiscard]] std::wstring extensionOf(const std::wstring& name) {
    const std::size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot + 1 == name.size()) {
        return L".";
    }
    return name.substr(dot);
}

/// The elements of `path` below `folder`; nothing when `path` does not lie inside it (pathIsWithin()'s comparison).
[[nodiscard]] std::optional<Elements> below(const Elements& path, const std::optional<Elements>& folder) {
    if (!folder || folder->empty() || folder->size() > path.size() ||
        !std::equal(folder->begin(), folder->end(), path.begin())) {
        return std::nullopt;
    }
    return Elements(path.begin() + static_cast<std::ptrdiff_t>(folder->size()), path.end());
}

[[nodiscard]] std::optional<Elements> foldedFolder(const std::optional<std::filesystem::path>& folder) {
    if (!folder || folder->empty()) {
        return std::nullopt;
    }
    return core::foldedPathElements(*folder);
}

[[nodiscard]] std::vector<std::wstring> foldedAll(const auto& texts) {
    std::vector<std::wstring> all;
    for (const auto& text : texts) {
        all.push_back(folded(text));
    }
    return all;
}

[[nodiscard]] bool contains(const std::vector<std::wstring>& list, const std::wstring& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

[[nodiscard]] ChangeScope none(std::string_view reason) {
    return ChangeScope{ScopeKind::None, {}, reason};
}
[[nodiscard]] ChangeScope everything(std::string_view reason) {
    return ChangeScope{ScopeKind::Everything, {}, reason};
}

} // namespace

const ScopeSystem* ChangeScopeMap::find(std::string_view name) const noexcept {
    for (const ScopeSystem& system : systems) {
        if (system.name.str() == name) {
            return &system;
        }
    }
    return nullptr;
}

ChangeScopeMap changeScopeMap(const catalog::LibraryProfile& profile, const ResolvedRoot& romRoot,
                              const ResolvedRoot& mediaRoot, const SystemsConfiguration& systems) {
    ChangeScopeMap map;
    map.esdeHome = profile.esdeHome;
    map.romRoot = romRoot.path;
    map.mediaRoot = mediaRoot.path;
    for (const SystemDefinition& definition : systems.systems) {
        ScopeSystem system;
        system.name = definition.name;
        system.romFolder = systemRomFolder(definition, romRoot, profile);
        system.extensions = definition.extensions;
        map.systems.push_back(std::move(system));
    }
    std::sort(map.systems.begin(), map.systems.end(),
              [](const ScopeSystem& a, const ScopeSystem& b) { return a.name.str() < b.name.str(); });
    return map;
}

std::string_view toString(ScopeKind kind) noexcept {
    switch (kind) {
    case ScopeKind::None:
        return "none";
    case ScopeKind::System:
        return "system";
    case ScopeKind::Everything:
        return "everything";
    }
    return "none";
}

ChangeScopeIndex::ChangeScopeIndex(ChangeScopeMap map) : m_map(std::move(map)) {
    if (!m_map.esdeHome.empty()) {
        m_settings = foldedFolder(m_map.esdeHome / "settings");
        m_custom = foldedFolder(m_map.esdeHome / "custom_systems");
        m_gamelists = foldedFolder(m_map.esdeHome / "gamelists");
    }
    m_media = foldedFolder(m_map.mediaRoot);
    m_rom = foldedFolder(m_map.romRoot);
    for (std::size_t index = 0; index < m_map.systems.size(); ++index) {
        const ScopeSystem& scope = m_map.systems[index];
        System system;
        system.index = index;
        system.folder = foldedFolder(scope.romFolder);
        system.name = folded(scope.name.str());
        system.extensions = foldedAll(scope.extensions);
        m_systems.push_back(std::move(system));
    }
}

const ChangeScopeIndex::System* ChangeScopeIndex::systemNamed(const std::wstring& name) const {
    for (const System& system : m_systems) {
        if (system.name == name) {
            return &system;
        }
    }
    return nullptr;
}

ChangeScope ChangeScopeIndex::scope(const std::filesystem::path& path) const {
    if (path.empty() || !path.is_absolute()) {
        return none("relative");
    }
    const std::optional<Elements> elements = core::foldedPathElements(path);
    if (!elements) {
        return none("outside");
    }
    const auto ofSystem = [&](const System& system, std::string_view reason) {
        return ChangeScope{ScopeKind::System, m_map.systems[system.index].name, reason};
    };
    // ES-DE's own configuration first: it may lie inside a ROM or media root (a portable installation).
    if (const auto settings = below(*elements, m_settings)) {
        return settings->size() == 1 && settings->front() == folded("es_settings.xml") ? everything("settings")
                                                                                        : none("settings");
    }
    if (const auto custom = below(*elements, m_custom)) {
        if (custom->empty() || (custom->size() == 1 && (custom->front() == folded("es_systems.xml") ||
                                                        custom->front() == folded("es_find_rules.xml")))) {
            return everything("custom-systems");
        }
        return none("custom-systems");
    }
    if (const auto gamelists = below(*elements, m_gamelists)) {
        if (gamelists->empty()) {
            return everything("gamelist");
        }
        const System* named = systemNamed(gamelists->front());
        if (named == nullptr) {
            return none("unknown-system");
        }
        if (gamelists->size() == 1 || (gamelists->size() == 2 && (*gamelists)[1] == folded("gamelist.xml"))) {
            return ofSystem(*named, "gamelist");
        }
        return none("gamelist");
    }
    if (const auto media = below(*elements, m_media)) {
        if (media->empty()) {
            return everything("root");
        }
        const System* named = systemNamed(media->front());
        if (named == nullptr) {
            return none("unknown-system");
        }
        if (media->size() <= 2) {
            return ofSystem(*named, "media");
        }
        static const std::vector<std::wstring> kMedia = [] {
            std::vector<std::wstring> all = foldedAll(kImageExtensions);
            for (const std::wstring& extension : foldedAll(kVideoExtensions)) {
                all.push_back(extension);
            }
            for (const std::wstring& extension : foldedAll(kManualExtensions)) {
                all.push_back(extension);
            }
            return all;
        }();
        const std::wstring extension = extensionOf(media->back());
        return extension == L"." || contains(kMedia, extension) ? ofSystem(*named, "media") : none("not-media");
    }
    // The system whose ROM folder holds the path most closely (the deepest folder).
    const System* owner = nullptr;
    std::optional<Elements> inside;
    for (const System& system : m_systems) {
        auto rest = below(*elements, system.folder);
        if (rest && (owner == nullptr || system.folder->size() > owner->folder->size())) {
            owner = &system;
            inside = std::move(rest);
        }
    }
    if (owner != nullptr) {
        if (inside->empty()) {
            return ofSystem(*owner, "rom-folder");
        }
        const std::wstring& name = inside->back();
        const std::wstring extension = extensionOf(name);
        if (extension == L"." || contains(owner->extensions, extension) || name == folded("noload.txt")) {
            return ofSystem(*owner, "rom");
        }
        return none("extension");
    }
    // A folder that holds system folders (the ROM root, a folder of a nested layout): what it holds may have moved.
    const bool holdsSystems = std::any_of(m_systems.begin(), m_systems.end(), [&](const System& system) {
        return system.folder && below(*system.folder, elements).has_value();
    });
    if (holdsSystems) {
        return everything("system-folders");
    }
    if (const auto rom = below(*elements, m_rom)) {
        return rom->empty() ? everything("root") : none("outside");
    }
    return none("outside");
}

ChangeScope changeScope(const ChangeScopeMap& map, const std::filesystem::path& path) {
    return ChangeScopeIndex(map).scope(path);
}

std::vector<core::WatchRoot> watchRoots(const ChangeScopeMap& map) {
    std::vector<core::WatchRoot> roots;
    const auto covered = [&](const std::filesystem::path& folder) {
        return std::any_of(roots.begin(), roots.end(), [&](const core::WatchRoot& root) {
            return core::pathIsWithin(folder, root.folder);
        });
    };
    const auto add = [&](std::string name, const std::filesystem::path& folder, bool subtree,
                         std::vector<std::filesystem::path> targets) {
        if (folder.empty() || covered(folder)) {
            return;
        }
        roots.push_back(core::WatchRoot{std::move(name), folder, subtree, std::move(targets)});
    };
    // ES-DE's configuration and gamelists: small folders on the ES-DE home's volume.
    if (!map.esdeHome.empty()) {
        add("settings", map.esdeHome / "settings", false, {map.esdeHome / "settings" / "es_settings.xml"});
        add("custom-systems", map.esdeHome / "custom_systems", false,
            {map.esdeHome / "custom_systems" / "es_systems.xml", map.esdeHome / "custom_systems" / "es_find_rules.xml"});
        std::vector<std::filesystem::path> gamelists;
        for (const ScopeSystem& system : map.systems) {
            gamelists.push_back(map.esdeHome / "gamelists" / core::pathFromUtf8(system.name.str()) / "gamelist.xml");
        }
        add("gamelists", map.esdeHome / "gamelists", true, std::move(gamelists));
    }
    if (map.romRoot) {
        std::vector<std::filesystem::path> targets{*map.romRoot};
        for (const ScopeSystem& system : map.systems) {
            if (system.romFolder && core::pathIsWithin(*system.romFolder, *map.romRoot)) {
                targets.push_back(*system.romFolder);
            }
        }
        add("rom", *map.romRoot, true, std::move(targets));
    }
    for (const ScopeSystem& system : map.systems) {
        if (system.romFolder) {
            add("system:" + system.name.str(), *system.romFolder, true, {*system.romFolder});
        }
    }
    if (map.mediaRoot) {
        std::vector<std::filesystem::path> targets{*map.mediaRoot};
        for (const ScopeSystem& system : map.systems) {
            const std::filesystem::path folder = *map.mediaRoot / core::pathFromUtf8(system.name.str());
            targets.push_back(folder);
            for (const catalog::MediaKind kind : catalog::kMediaKinds) {
                targets.push_back(folder / core::pathFromUtf8(catalog::toString(kind)));
            }
        }
        add("media", *map.mediaRoot, true, std::move(targets));
    }
    return roots;
}

} // namespace rv::esde
