#pragma once

// ES-DE's launch commands translated into launch requests (ADR-0008 §8), as ES-DE 3.4.1's FileData::launchGame and
// findEmulator read them on Windows, but split before anything is expanded: a command is split into argv elements the
// way CommandLineToArgvW splits a command line (parseCommandTemplate()), each token is then replaced by its value, and
// a value is never split or expanded again, so that a ROM path holding spaces, quotes, '%', '^' or ';' stays one
// argument. Nothing is started and nothing is written: the result is a request for rv_launcher, or the blockers that say
// why the game cannot start (catalog::LaunchTranslation).
//
// The program is decided on the unexpanded command: it must be exactly one %EMULATOR_X% element, found through the
// find rules of X, or a literal absolute path whose only tokens are '~' and %ESPATH%. Command interpreters and launchers
// are refused by name even when a find rule finds them (launcher::isRefusedProgram: cmd, powershell, explorer, ...), and
// the request must pass launcher::validateLaunchRequest (a PE .exe or .com, the working folder there).
//
// The find rules are evaluated as ES-DE evaluates them, on the machine core::ISystemProbe describes (findEmulator(),
// findCore()): every winregistrypath entry (App Paths: HKCU, and HKLM only when the HKCU key does not exist), then every
// winregistryvalue entry ("<key>\<value>|<suffix>": the suffix appended verbatim to the value), every systempath entry
// (the executable search: the ES-DE install folder, System32, the Windows folder, then PATH's absolute entries; never
// Retroverse's folder or the current one, unlike ES-DE's SearchPathW, since a ROM tree may hold emulators named like the
// entries), every staticpath entry ('~', %ESPATH% and %ROMPATH% expanded; a '*' in the last element matched as ES-DE's
// regular expression, case-sensitively, the first match in ordinal order winning; a '|' replacement program requires
// launcher configuration); for a core, each corepath entry with %EMUPATH% the program's folder. The custom find rules
// come first (loadFindRules()).
//
// Tokens and their values, for a game "<ROM folder>\<relative path>" (ES-DE's text of it: ROMDirectory as configured,
// the system's <path> with %ROMPATH% expanded, the relative path as listed on disk; '\' between the elements):
//   %ROM%, %ROMRAW%          the game's path                         (ES-DE quotes %ROM% when it holds a space: one
//                                                                     argument either way)
//   %ROMPATH%                the ROM root (ROMDirectory)
//   %BASENAME%               the game's name without its last extension (a folder's whole name)
//   %FILENAME%               %BASENAME% and the last extension ("." when there is none), as ES-DE builds it
//   %GAMEDIR%, %GAMEDIRRAW%  the folder holding the game
//   %EMUDIR%                 the program's folder
//   %ESPATH%                 the ES-DE install folder ("requires launcher configuration" when it is not known)
//   ~                        ES-DE's home folder, the ES-DE home's parent (ES-DE's text of it, '/' separators)
//   %CORE_X%\<file>          the core: <corepath entry>\<file>, the first that exists ("core not found" otherwise)
//   %STARTDIR%=<folder>      the working folder ('~', %EMUDIR%, %GAMEDIR% in it); without it, the program's folder
//                            (ES-DE leaves emulators in its own working folder; Retroverse keeps them out of it)
//   %HIDEWINDOW%             a console program runs without a window (LaunchRequest::hideConsoleWindow)
//   %INJECT%=<file>          the element (or its value, glued to a prefix) is dropped when the file (relative to the
//                            game's folder, %BASENAME% expanded) is missing, empty or over 4,096 characters, as ES-DE
//                            then injects nothing; a file ES-DE would inject requires launcher configuration
// Anything else requires launcher configuration with the token named: %RUNINBACKGROUND%, %ESCAPESPECIALS%,
// %PRECOMMAND_X%, %GAMEENTRYDIR% and %EMUPATH% in a command, %STARTDIR% or %INJECT% without '=', a second program or core,
// unknown %X% text (%RPCS3_GAMEID%, the tokens of ES-DE's other platforms), placeholder commands.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/launch_resolution.hpp>
#include <rv/catalog/library.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/system_probe.hpp>
#include <rv/esde/find_rules.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>

namespace rv::esde {

/// What a translation reads: the library (its ES-DE home gives '~', its install folder %ESPATH%), the ROM root
/// (%ROMPATH%, the systems' ROM folders), the find rules and the machine. The pointers must outlive the calls.
struct TranslationContext {
    catalog::LibraryProfile profile;
    ResolvedRoot romRoot;
    const FindRules* findRules = nullptr;
    const core::ISystemProbe* probe = nullptr;
};

/// The largest %INJECT% content ES-DE injects (characters, line ends removed); a longer one injects nothing.
inline constexpr std::size_t kMaxInjectCharacters = 4096;

/// What a find-rule search found.
struct FindResult {
    /// The file and the rule and entry that found it.
    std::optional<catalog::FoundLocation> found;
    /// For a core: its text as the command gets it (ES-DE's: the corepath entry expanded, '\' and the file name).
    std::string text;
    /// What was tried and why it failed, in ES-DE's order ("winregistrypath \"retroarch.exe\": not registered").
    std::vector<std::string> attempts;
    /// The search stopped at what ES-DE would do and Retroverse does not decide: a staticpath entry whose path exists
    /// and whose '|' replaces the program, or an entry that needs %ESPATH% (an unknown install folder) or %ROMPATH%
    /// (an unresolved ROM root) before anything was found. Requires launcher configuration, with this reason.
    std::optional<std::string> undecided;
    /// No find rules define the name.
    bool noRules = false;
};

/// Looks for the program of %EMULATOR_<name>% (`name` compared exactly) through its find rules, as the header comment
/// describes.
[[nodiscard]] FindResult findEmulator(const TranslationContext& context, std::string_view name);

/// Looks for the core file `coreFile` ("bsnes_libretro.dll", the text after %CORE_<name>% and its separator) through
/// the corepath rules of `name`, %EMUPATH% being `programFolder` (the program's folder).
[[nodiscard]] FindResult findCore(const TranslationContext& context, std::string_view name,
                                  const std::filesystem::path& programFolder, std::string_view coreFile);

/// Translates `command` of `system` for `game` (catalog::LaunchGame::relativePath as listed on disk), or for the
/// system when `game` is null: the program, the core and every token that does not depend on a game are checked, the
/// %INJECT% files and the %STARTDIR% folders that do are left to each game (a note says so), and no request is built.
/// For a game, the request is built once the program is found (catalog::LaunchTranslation::request) and checked with
/// launcher::validateLaunchRequest(); its label is "<title> with <command label>".
[[nodiscard]] catalog::LaunchTranslation translateCommand(const TranslationContext& context,
                                                          const SystemDefinition& system,
                                                          const SystemCommand& command,
                                                          const catalog::LaunchGame* game);

/// The catalog's port over an ES-DE installation's systems: translateCommand() of a system's command by its label.
class EsdeCommandTranslator final : public catalog::ICommandTranslator {
public:
    /// `systems` and the context's pointers must outlive the translator.
    EsdeCommandTranslator(TranslationContext context, const SystemsConfiguration& systems);

    [[nodiscard]] std::optional<std::vector<std::string>> commandLabels(
        const catalog::SystemName& system) const override;
    [[nodiscard]] catalog::LaunchTranslation translate(const catalog::SystemName& system, std::string_view label,
                                                       const catalog::LaunchGame* game) const override;

    [[nodiscard]] const TranslationContext& context() const noexcept { return m_context; }

private:
    TranslationContext m_context;
    const SystemsConfiguration* m_systems = nullptr;
};

/// A file or folder below a ROM folder as the file system spells it.
struct ListedPath {
    /// The relative path with each element's name as the folder lists it ('/' between the elements): what ES-DE's
    /// %ROM% ends with.
    std::string relativePath;
    bool directory = false;
};

/// The spelling on disk of `relativePath` (UTF-8, '/' or '\' between the elements) below `romFolder`, each element
/// looked up by its name (FindFirstFileExW through the \\?\ form: any length, never a wildcard). Errors: NotFound (an
/// element is not there), Unavailable (its drive or share cannot be reached), InvalidArgument (an empty element, "." or
/// "..", a wildcard or a character Windows names cannot hold, text that is not UTF-8), IoError.
[[nodiscard]] Expected<ListedPath> listedPath(const std::filesystem::path& romFolder, std::string_view relativePath);

} // namespace rv::esde
