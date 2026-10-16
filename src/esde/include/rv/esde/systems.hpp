#pragma once

// es_systems.xml (ADR-0008 §3): the game systems ES-DE knows, read as ES-DE 3.4.1's SystemData::loadConfig reads them.
// Two files: the user's <ES-DE home>/custom_systems/es_systems.xml first, then the bundled
// resources/systems/windows/es_systems.xml (bundledResource(): the ES-DE home's resources/ before the install
// folder's); a <loadExclusive/> beside the custom file's <systemList> leaves the bundled file out. A name defined twice
// (a custom system replacing the bundled one, or a repeated name) is the first definition's; ES-DE tries the later ones
// only when the earlier ones find no game in their folder, so they are kept for the ROM scan (I2 step 7). Each system:
// its name, full name, ROM folder (%ROMPATH% resolved against the ROM root, kept as written too), extensions used
// verbatim (split on blanks and commas, as ES-DE's readList splits them: "." means "no extension", upstream quirks
// such as pcfx's "CUE" stay as they are and match no file), commands with their labels in order (the alternative
// emulators; ES-DE's label rules applied, placeholder commands flagged), platforms and theme. The commands are kept for
// the launch translation (I2 step 10); the domain sees the systems as catalog::SystemInfo values (systemInfos()).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog/system.hpp>
#include <rv/core/expected.hpp>
#include <rv/esde/settings.hpp>

namespace rv::esde {

/// Which file defined a system or a find rule.
enum class ConfigOrigin : std::uint8_t {
    Bundled, ///< ES-DE's resources/systems/windows (bundledResource())
    Custom,  ///< <ES-DE home>/custom_systems
};

/// "bundled", "custom".
[[nodiscard]] std::string_view toString(ConfigOrigin origin) noexcept;

/// One <command> of a system.
struct SystemCommand {
    /// Its label attribute, as written: the name ES-DE's alternative emulators choose it by (a gamelist's
    /// <alternativeEmulator>, a game's <altemulator>). Empty for a system's single command without one.
    std::string label;
    /// The command as written, unexpanded (parseCommandTemplate() splits it).
    std::string text;
    /// "placeholder" in the text, in any case ("PLACEHOLDER %ROM%"): an upstream system nothing can launch yet, which
    /// ES-DE skips when it creates the system folders.
    bool placeholder = false;

    bool operator==(const SystemCommand&) const = default;
};

/// A system as a file defines it.
struct SystemDefinition {
    catalog::SystemName name;
    /// <fullname>, line feeds removed as ES-DE removes them.
    std::string fullName;
    /// <systemsortname> as written; empty when none (ES-DE then sorts by the full name).
    std::string sortName;
    /// <path> as written ("%ROMPATH%\gb").
    std::string path;
    /// <extension> split on blanks and commas, each as written, in order (".zip", ".ZIP", ".", "CUE").
    std::vector<std::string> extensions;
    /// The commands ES-DE keeps, in order (at least one): the first is the default; labels are unique.
    std::vector<SystemCommand> commands;
    /// <platform> lower-cased and split on blanks and commas, in order ("pc, pcwindows" gives "pc" and "pcwindows").
    std::vector<std::string> platforms;
    /// <theme>, or the name when it is missing or empty.
    std::string theme;
    ConfigOrigin origin = ConfigOrigin::Bundled;

    /// The command labelled `label` (compared exactly, as ES-DE does), or null.
    [[nodiscard]] const SystemCommand* findCommand(std::string_view label) const noexcept;

    bool operator==(const SystemDefinition&) const = default;
};

/// The systems of an ES-DE installation.
struct SystemsConfiguration {
    /// One per name, the first definition ES-DE reads (the custom file's before the bundled one's), sorted by name
    /// (ordinal).
    std::vector<SystemDefinition> systems;
    /// The later definitions of a name, in ES-DE's reading order: the bundled ones a custom definition replaced, and
    /// names a file repeats. ES-DE falls back to them, in this order, when every earlier definition of the name finds
    /// no game (its folder missing or holding no file it lists): the ROM scan's to decide.
    std::vector<SystemDefinition> fallbacks;
    /// The bundled file (whether or not <loadExclusive/> left it out).
    std::filesystem::path bundledFile;
    /// The custom file, when it exists.
    std::optional<std::filesystem::path> customFile;
    /// The custom file holds <loadExclusive/>: the bundled file was not read.
    bool loadExclusive = false;
    /// Definitions ES-DE skips ("system-skipped": no name, no full name, path, extension or command), names Retroverse
    /// cannot use as a folder name ("system-name-invalid"), ES-DE's command label rules ("command-label-missing":
    /// commands left out; "command-label-duplicate": a command left out), extensions without a leading dot
    /// ("extension-without-dot", info), a system without a platform ("platform-missing", info), replaced and repeated
    /// names ("system-replaced", info; "system-repeated", warning), a <loadExclusive/> in the bundled file
    /// ("load-exclusive-ignored").
    std::vector<catalog::Issue> issues;

    /// The system named `name` (its effective definition), or null.
    [[nodiscard]] const SystemDefinition* find(std::string_view name) const noexcept;
    /// The commands of all the systems (their effective definitions).
    [[nodiscard]] std::size_t commandCount() const noexcept;

    bool operator==(const SystemsConfiguration&) const = default;
};

/// The largest es_systems.xml read (ES-DE 3.4.1's is 168 KiB).
inline constexpr std::uintmax_t kMaxSystemsBytes = 16ull * 1024ull * 1024ull;

/// Reads the systems of the library `profile` describes (its ES-DE home and install folder). Errors: a relative ES-DE
/// home (InvalidArgument), no bundled es_systems.xml (NotFound: ES-DE refuses to start without it), a file that cannot
/// be read (Unavailable, Conflict, OutOfRange, IoError) or parsed, or without <systemList> (ParseError: ES-DE refuses
/// to start then too).
[[nodiscard]] Expected<SystemsConfiguration> loadSystems(const catalog::LibraryProfile& profile);

/// Reads the given files, as loadSystems() does with the files it finds (tests, tools): `customFile` first when given.
[[nodiscard]] Expected<SystemsConfiguration> loadSystemsFiles(const std::optional<std::filesystem::path>& customFile,
                                                              const std::filesystem::path& bundledFile);

/// The ROM folder of `system` as ES-DE builds it: %ROMPATH% replaced with the ROM root's text (ResolvedRoot::esdeText),
/// then '~' with ES-DE's home folder (profile.esdeHome's parent), so that a '~' of ROMDirectory is replaced twice, as
/// ES-DE replaces it; '\' between the elements. An override (RootSource::Override), which ES-DE never sees, is put in
/// after the '~' of the path: a '~' of its own ("D:\EMULAT~1") stays. Nothing when the path uses %ROMPATH% and the root
/// is not resolved, or does not give an absolute folder.
[[nodiscard]] std::optional<std::filesystem::path> systemRomFolder(const SystemDefinition& system,
                                                                   const ResolvedRoot& romRoot,
                                                                   const catalog::LibraryProfile& profile);

/// The folders of a library that Retroverse only reads (ADR-0008 §6, §7), which its user data directory must stay out
/// of (core::checkUserDataDirectory()): the ES-DE home, the install folder when known, the ROM and media roots when
/// resolved, then the ROM folder of every system of `systems` (the definitions and their fallbacks) that lies outside
/// them (an absolute <path>, or one using '~'), since the ROM scan reads those too. A folder inside one listed already
/// is left out. `romRoot`, `mediaRoot` and `systems` may be missing (not known: settings or systems that cannot be
/// read); the folders known are still given.
[[nodiscard]] std::vector<std::filesystem::path> readOnlyFolders(const catalog::LibraryProfile& profile,
                                                                 const std::optional<ResolvedRoot>& romRoot,
                                                                 const std::optional<ResolvedRoot>& mediaRoot,
                                                                 const SystemsConfiguration* systems);

/// readOnlyFolders() of the library `profile` describes, its settings and systems read (readSettings(),
/// resolveRomRoot(), resolveMediaRoot(), loadSystems()); what cannot be read is left out, the overrides need no
/// settings. Nothing is written.
[[nodiscard]] std::vector<std::filesystem::path> readOnlyFolders(const catalog::LibraryProfile& profile);

/// The systems as the domain keeps them, one per name, by name: name, full name, platforms, theme, the path as written
/// and the ROM folder (systemRomFolder()), whether that folder is there (checkFolder(): Available, or SourceUnavailable
/// with FolderMissing, Unreachable or NotListable; FolderMissing without a folder when it cannot be resolved). The
/// gamelist's state and alternative emulator are left as SystemInfo's defaults: the gamelists are read later. Issues:
/// "rom-folder-unresolved" for a path that gives no absolute folder on its own (an unresolved ROM root is reported by
/// resolveRomRoot()).
[[nodiscard]] std::vector<catalog::SystemInfo> systemInfos(const SystemsConfiguration& systems,
                                                           const ResolvedRoot& romRoot,
                                                           const catalog::LibraryProfile& profile,
                                                           std::vector<catalog::Issue>& issues);

/// The gamelist ES-DE reads for the system `name` (SystemData::getGamelistPath): <romFolder>\gamelist.xml when
/// LegacyGamelistFileLocation is on and that file exists, else <ES-DE home>\gamelists\<name>\gamelist.xml (which may
/// not exist: the system has no gamelist).
[[nodiscard]] std::filesystem::path gamelistFile(const catalog::SystemName& name,
                                                 const std::optional<std::filesystem::path>& romFolder,
                                                 const EsdeSettings& settings,
                                                 const std::filesystem::path& esdeHome);

} // namespace rv::esde
