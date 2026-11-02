#pragma once

// What a changed path means to the library (ADR-0018 §10): a system to refresh, every system, or nothing. The file
// watcher (core::IChangeWatcher) reports paths; changeScope() maps each over a ChangeScopeMap made from the library's
// configuration (changeScopeMap()), lexically, without touching the disk:
//  - es_settings.xml, the custom systems' es_systems.xml and es_find_rules.xml: every system (the configuration moved);
//  - <ES-DE home>/gamelists/<system>/gamelist.xml, or that system's gamelist folder itself: the system;
//  - <media root>/<system>/... : the system for its folder, its kind folders and a file with a media extension (or none,
//    a folder): an image, a video or a manual (mediaExtensions());
//  - inside a system's ROM folder (the deepest that holds it): the folder itself, a name with one of the system's
//    extensions (compared ignoring case: ".SFC" counts for ".sfc"), a name without one or a folder the watcher saw (one
//    moved in, its own files unreported), noload.txt:
//    the system; anything else (an emulator's save or state beside a ROM, "game.srm") is nothing;
//  - a folder that holds system folders (the ROM root, a folder of a nested layout), the media root, the gamelists
//    folder: every system;
//  - anything else, a relative path included: nothing.
// watchRoots() names what the watcher watches: the ROM root, each system folder outside it, the media root, the ES-DE
// home's gamelists, custom_systems and settings folders, each with the entries its fallback poll looks at (the system
// folders' level, the gamelists' files: never a walk).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/core/change_watcher.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>

namespace rv::esde {

/// A system as the scope sees it.
struct ScopeSystem {
    catalog::SystemName name;
    /// Its ROM folder (systemRomFolder()); nothing when it cannot be resolved.
    std::optional<std::filesystem::path> romFolder;
    /// Its extensions, as es_systems.xml writes them.
    std::vector<std::string> extensions;

    bool operator==(const ScopeSystem&) const = default;
};

/// The library's folders and systems, for changeScope() and watchRoots().
struct ChangeScopeMap {
    std::filesystem::path esdeHome;
    std::optional<std::filesystem::path> romRoot;
    std::optional<std::filesystem::path> mediaRoot;
    /// One per system name (the effective definitions), by name.
    std::vector<ScopeSystem> systems;

    [[nodiscard]] const ScopeSystem* find(std::string_view name) const noexcept;
    bool operator==(const ChangeScopeMap&) const = default;
};

/// The map of a library: `profile`'s ES-DE home, the roots as resolved, the systems' folders and extensions.
[[nodiscard]] ChangeScopeMap changeScopeMap(const catalog::LibraryProfile& profile, const ResolvedRoot& romRoot,
                                            const ResolvedRoot& mediaRoot, const SystemsConfiguration& systems);

/// What a change touches.
enum class ScopeKind : std::uint8_t {
    None,       ///< nothing the library shows
    System,     ///< one system
    Everything, ///< every system (the configuration, a root, a folder of system folders)
};

/// "none", "system", "everything".
[[nodiscard]] std::string_view toString(ScopeKind kind) noexcept;

struct ChangeScope {
    ScopeKind kind = ScopeKind::None;
    /// For System.
    catalog::SystemName system;
    /// Why (for the log and the tests): "settings", "custom-systems", "gamelist", "media", "rom", "rom-folder",
    /// "system-folders", "root", "extension" (None: a ROM folder's file without a system extension), "not-media",
    /// "unknown-system", "outside" (None: in no watched folder), "relative".
    std::string_view reason;

    bool operator==(const ChangeScope&) const = default;
};

/// changeScope() over a map whose folders, names and extensions are folded once: what the watcher's many changes are
/// mapped with.
class ChangeScopeIndex {
public:
    explicit ChangeScopeIndex(ChangeScopeMap map);

    /// The scope of a change of `path` (absolute; lexical, as above). `directory`: the path is known to be a folder (an
    /// added or renamed one): inside a ROM or media folder it counts whatever its name, a dotted one included ("Collection
    /// v2.0", "Game (Rev 1.1)").
    [[nodiscard]] ChangeScope scope(const std::filesystem::path& path, bool directory = false) const;
    [[nodiscard]] const ChangeScopeMap& map() const noexcept { return m_map; }

private:
    using Elements = std::vector<std::wstring>;
    struct System {
        std::size_t index = 0; ///< in m_map.systems
        std::optional<Elements> folder;
        std::wstring name;
        std::vector<std::wstring> extensions;
    };

    [[nodiscard]] const System* systemNamed(const std::wstring& folded) const;

    ChangeScopeMap m_map;
    std::optional<Elements> m_settings;
    std::optional<Elements> m_custom;
    std::optional<Elements> m_gamelists;
    std::optional<Elements> m_media;
    std::optional<Elements> m_rom;
    std::vector<System> m_systems;
};

/// The scope of a change of `path` (absolute; lexical, as above): ChangeScopeIndex(map).scope(path, directory).
[[nodiscard]] ChangeScope changeScope(const ChangeScopeMap& map, const std::filesystem::path& path,
                                      bool directory = false);

/// The roots the watcher watches for `map`, named "rom", "system:<name>", "media", "gamelists", "custom-systems",
/// "settings"; a folder inside one listed before is left out (its changes reach that one).
[[nodiscard]] std::vector<core::WatchRoot> watchRoots(const ChangeScopeMap& map);

} // namespace rv::esde
