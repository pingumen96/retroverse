#pragma once

// Emulator profiles (specification §11.2, ADR-0008 §6 and §8): how Retroverse starts an emulator it was told about,
// beside the ES-DE commands the catalog translates (I2 step 10). JSON with comments: the shipped defaults are
// <asset root>/launcher/emulators.json, the user's profiles <user data>/config/emulators.json, whose profiles replace
// the shipped ones with the same id and add new ones. Executables are located on the machine through
// core::ISystemProbe (App Paths, then paths with '~' and {env:NAME}), so that no machine path appears in code or
// shipped files (specification §11.1). docs/i2/step-5.md describes the format with an example.
//
// Format, schemaVersion 2 (a document of schemaVersion 1 is read too: the same without "programs"):
//
//     {
//       "schemaVersion": 2,
//       "profiles": [
//         {
//           "id": "retroarch",                    // required: [a-z0-9][a-z0-9._-]*, at most 64 characters
//           "displayName": "RetroArch",           // default: the id
//           "executable": {                       // required: one path (a string), or this object
//             "appPaths": "retroarch.exe",        //   an App Paths name (HKCU, else HKLM), tried first
//             "paths": ["~\\AppData\\Roaming\\RetroArch\\retroarch.exe"]   // then these, in order
//           },
//           "arguments": ["-L", "{var:core}", "{rom}"], // required: the argv template (arg_template.hpp)
//           "workingDirectory": "{rom_dir}",      // a template; default: the executable's folder (ADR-0008 §8)
//           "environment": {"NAME": "value", "GONE": null}, // sets (a template) or removes variables
//           "wait": "direct",                     // direct (default), tree or confirm (launch_request.hpp)
//           "windowTimeoutSeconds": 30,           // 0: none; default 30 (LaunchTimeouts::windowTimeout)
//           "hideConsoleWindow": false,           // console programs without a window (ES-DE's %HIDEWINDOW%)
//           "variables": {"cores": "{emulator_dir}\\cores"},
//           "requiredFiles": ["{var:core}"],      // files that must exist (templates): a RetroArch core
//           "systems": {"gb": {"variables": {"core": "{var:cores}\\gambatte_libretro.dll"}}},
//           "verified": false                     // informational: a launch was tested (specification §11.3)
//         }
//       ],
//       "systemChoices": {                        // optional (I2 step 10): how a system launches, by Retroverse's
//         "snes": {"command": "bsnes"},           //   choice: an ES-DE command of the system, by its label,
//         "gb": {"profile": "retroarch"}          //   or one of these profiles, by its id
//       },
//       "programs": [                             // optional (I11 step 12, ADR-0020 §1): what Retroverse knows of a
//         {                                       //   program, whichever command or profile starts it
//           "id": "dolphin",                      // required: as a profile's id
//           "displayName": "Dolphin",             // default: the id
//           "files": ["Dolphin.exe"],             // required: the program's file names ('*' and '?' allowed),
//                                                 //   compared with the file-system casing
//           "window": {                           // how its windows rank (window_choice.hpp), on every launch
//             "prefer": [{"titleContains": " | "}],
//             "skip": [{"classContains": "Splash"}],
//             "minWidth": 200, "minHeight": 150
//           },
//           "vr": {                               // a launch made in VR only
//             "addArguments": ["-C", "Dolphin.Display.Fullscreen=False"], // right after the program, as written
//             "dropArguments": ["--fullscreen"],  // the launch's elements equal to one of these, removed
//             "appendConfig": {                   // a file Retroverse writes, passed with the program's option
//               "option": "--appendconfig",
//               "file": "retroarch.cfg",          //   <user data>/launcher/vr/<file>
//               "joinWith": "|",                  //   the option already given: its value, this, then the file
//               "lines": ["video_windowed_fullscreen = \"true\""]
//             },
//             "inGame": "heavy",                  // light (default) or heavy: the scene renders at less meanwhile
//             "heavyWith": ["dolphin_libretro.dll"] // heavy when an argument's file name matches one of these
//           },
//           "verified": false                     // informational: tried in the headset
//         }
//       ]
//     }
//
// Paths of "executable": absolute once expanded; a leading '~' is the user's home folder (ISystemProbe::homeFolder),
// {env:NAME} tokens of the allow-list are expanded (no other token), and the last element may hold the wildcards '*'
// and '?' (the first match in sorted order).
//
// "systemChoices" is Retroverse's choice for a system (ADR-0008 §8: after the game's own choices, before the gamelist's
// <alternativeEmulator>); the user's document replaces the shipped one's per system. A choice is stored as a label or an
// id, never as a copied command: the catalog's launch resolution translates it again from the current es_systems.xml
// at every refresh and launch, and a label or id that no longer exists makes the system "requires launcher
// configuration", naming it. Choices made through retroverse-cli (assign) and per-game choices live in the database.
//
// "programs" (ADR-0020 §1) describe programs, not launches: a launch is matched to one by its executable's file name,
// whether an ES-DE command or a profile made it (EmulatorProfiles::programFor()), and vr_launch.hpp applies it. The
// user's programs replace the shipped ones with the same id, and are matched first. Retroverse never writes a program's
// own configuration: a launch in VR changes the arguments and passes Retroverse's own file.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/core/system_probe.hpp>
#include <rv/launcher/arg_template.hpp>
#include <rv/launcher/environment.hpp>
#include <rv/launcher/launch_request.hpp>
#include <rv/launcher/window_choice.hpp>

namespace rv::core {
class Paths;
} // namespace rv::core

namespace rv::launcher {

/// The version written; kOldestEmulatorProfilesSchemaVersion and later are read.
inline constexpr int kEmulatorProfilesSchemaVersion = 2;
inline constexpr int kOldestEmulatorProfilesSchemaVersion = 1;
/// The shipped profiles, relative to the asset root.
inline constexpr std::string_view kShippedProfilesAsset = "launcher/emulators.json";
/// The user's profiles, in the configuration folder (core::Paths::configDir()).
inline constexpr std::string_view kUserProfilesFileName = "emulators.json";
/// The longest window timeout a profile may set.
inline constexpr std::chrono::seconds kMaxWindowTimeout{24 * 60 * 60};
/// The longest profile id.
inline constexpr std::size_t kMaxProfileIdLength = 64;

/// Where a profile comes from.
enum class ProfileOrigin : std::uint8_t {
    Shipped, ///< <asset root>/launcher/emulators.json
    User,    ///< <user data>/config/emulators.json
};

/// "shipped", "user".
[[nodiscard]] std::string_view toString(ProfileOrigin origin) noexcept;

/// Where a profile's executable is: App Paths first, then each path in order; the first existing file wins.
struct ExecutableSpec {
    /// An executable's App Paths name ("retroarch.exe": core::appPath, HKCU, else HKLM); empty: none.
    std::string appPathsName;
    /// Paths as written (see the format above).
    std::vector<std::string> paths;

    friend bool operator==(const ExecutableSpec&, const ExecutableSpec&) = default;
};

/// What a profile sets for one system it serves.
struct SystemSettings {
    /// The system's variables, between the launch's and the profile's (a RetroArch core per system).
    TemplateVariables variables;

    friend bool operator==(const SystemSettings&, const SystemSettings&) = default;
};

/// One emulator profile, validated: every template parses and names only allowed environment variables, environment
/// names pass validateOverride(), ids and names are well formed. Whether the variables a launch needs are defined is
/// known only for a system and a launch: launch_plan.hpp.
struct EmulatorProfile {
    /// [a-z0-9][a-z0-9._-]*: stored with per-game and per-system choices (ADR-0008 §8), so lower case only.
    std::string id;
    std::string displayName;
    ExecutableSpec executable;
    /// argv[1..] as a template: one argument per element (arg_template.hpp).
    std::vector<std::string> arguments;
    /// A template naming the working directory; empty: the executable's folder (ADR-0008 §8).
    std::string workingDirectory;
    /// Environment overrides; values are templates.
    std::vector<EnvironmentOverride> environment;
    WaitStrategy waitStrategy = WaitStrategy::DirectProcess;
    /// LaunchTimeouts::windowTimeout of its launches; zero disables the rule (windowless console emulators).
    std::chrono::milliseconds windowTimeout = LaunchTimeouts{}.windowTimeout;
    /// LaunchRequest::hideConsoleWindow of its launches.
    bool hideConsoleWindow = false;
    /// The profile's variables, the last scope of {var:NAME}.
    TemplateVariables variables;
    /// The systems it serves (ES-DE system names, compared exactly) with their settings. A launch for another system
    /// is planned with the launch's and the profile's variables alone.
    std::map<std::string, SystemSettings, std::less<>> systems;
    /// Templates naming files that must exist for a launch (a RetroArch core): readiness "core not found" otherwise.
    std::vector<std::string> requiredFiles;
    /// Informational: a launch with it was tested. Never required to launch (ADR-0008 §8).
    bool verified = false;
    ProfileOrigin origin = ProfileOrigin::Shipped;
    /// The file it was read from (UTF-8), for messages.
    std::string source;

    /// True when `system` is one of systems.
    [[nodiscard]] bool serves(std::string_view system) const;
    /// The variables of `system`, nullptr when the profile does not serve it.
    [[nodiscard]] const TemplateVariables* systemVariables(std::string_view system) const;

    friend bool operator==(const EmulatorProfile&, const EmulatorProfile&) = default;
};

/// A problem found while loading profiles: the file, the profile when one is concerned, what is wrong and where.
struct ProfileIssue {
    /// The document (UTF-8 path).
    std::string file;
    /// The profile's id; empty when the document itself is at fault or the profile has no valid id.
    std::string profileId;
    /// What is wrong; its context is "<file>:<JSON pointer>" ("emulators.json:/profiles/1/arguments/2"), or the file
    /// alone when the document as a whole is at fault.
    Error error;

    /// "profile 'retroarch': <error>" or the error alone.
    [[nodiscard]] std::string toString() const;
};

/// What a stored launch choice names (ADR-0008 §8).
enum class LaunchChoiceKind : std::uint8_t {
    EsdeCommand, ///< one of the system's ES-DE commands, by its label (es_systems.xml's <command label>)
    Profile,     ///< a Retroverse emulator profile, by its id
};

/// "command", "profile": the keys of a choice in emulators.json.
[[nodiscard]] std::string_view toString(LaunchChoiceKind kind) noexcept;

/// A choice of how a system or a game launches, as Retroverse stores it: an ES-DE command label of the system or a
/// profile id, never a copy of a command, so that the current configuration translates it at every refresh and launch.
struct LaunchChoice {
    LaunchChoiceKind kind = LaunchChoiceKind::EsdeCommand;
    /// The label (compared exactly, as ES-DE compares labels) or the profile id.
    std::string value;

    friend bool operator==(const LaunchChoice&, const LaunchChoice&) = default;
};

/// The per-system choices of a document: ES-DE system name (compared exactly) -> choice.
using SystemChoices = std::map<std::string, LaunchChoice, std::less<>>;

/// How much of the GPU a program takes while its game shows in VR (ADR-0020 §1): a heavy one leaves the scene less
/// (its render scale's cap is lower while the game shows).
enum class InGameLoad : std::uint8_t {
    Light,
    Heavy,
};

/// "light", "heavy".
[[nodiscard]] std::string_view toString(InGameLoad load) noexcept;

/// The longest name of a VR append-file, and the longest of its lines.
inline constexpr std::size_t kMaxAppendFileNameLength = 64;
inline constexpr std::size_t kMaxAppendLineLength = 1024;

/// A file Retroverse writes for a launch in VR and passes with the program's own option (RetroArch's --appendconfig):
/// the program reads it over its own configuration, which Retroverse never writes.
struct AppendConfig {
    /// The program's option, an argument of its own ("--appendconfig"); the file's path follows it.
    std::string option;
    /// The file's name under <user data>/launcher/vr: letters, digits, '.', '_', '-', not starting with '.'.
    std::string fileName;
    /// When the launch already passes `option`: its value, this text, then the file's path (RetroArch reads "a|b");
    /// empty: the option and the file are added in front anyway.
    std::string joinWith;
    /// The file's lines, each ended by a line feed (no line break inside one).
    std::vector<std::string> lines;

    friend bool operator==(const AppendConfig&, const AppendConfig&) = default;
};

/// What a program does differently when its game is played in VR.
struct VrSettings {
    /// Added right after the program, before the launch's own arguments, as written (no template).
    std::vector<std::string> addArguments;
    /// The launch's arguments equal to one of these are removed ("-f", "--fullscreen").
    std::vector<std::string> dropArguments;
    std::optional<AppendConfig> appendConfig;
    InGameLoad inGame = InGameLoad::Light;
    /// File names ('*' and '?' allowed): an argument naming one of them makes the launch heavy (a RetroArch core).
    std::vector<std::string> heavyWith;

    friend bool operator==(const VrSettings&, const VrSettings&) = default;
};

/// A program Retroverse knows (ADR-0020 §1), whichever command or profile starts it.
struct ProgramProfile {
    /// As EmulatorProfile::id.
    std::string id;
    std::string displayName;
    /// Its file names, '*' and '?' allowed ("duckstation-qt-*.exe").
    std::vector<std::string> files;
    /// How its windows rank, on every launch (the window followed, the foreground).
    WindowRules window;
    VrSettings vr;
    /// Informational: its launch in VR was tried in the headset.
    bool verified = false;
    ProfileOrigin origin = ProfileOrigin::Shipped;
    std::string source;

    /// True when `executable`'s file name matches one of files (the file-system casing, as NTFS compares names).
    [[nodiscard]] bool matches(const std::filesystem::path& executable) const;

    friend bool operator==(const ProgramProfile&, const ProgramProfile&) = default;
};

/// The profiles of one document, in their order, its per-system choices, its programs, and an issue for each problem: a
/// profile, a choice or a program with an error is left out (the others are kept); a document that cannot be read, is
/// not an object, has an unknown schemaVersion or no "profiles" array gives no profile, no choice and no program.
struct ProfileDocument {
    std::vector<EmulatorProfile> profiles;
    /// "systemChoices": {"<system>": {"command": "<label>"} or {"profile": "<id>"}}. Whether the label or the profile
    /// exists is the launch resolution's to tell (the profile may come from the other document).
    SystemChoices systemChoices;
    /// "programs" (schemaVersion 2).
    std::vector<ProgramProfile> programs;
    std::vector<ProfileIssue> issues;
};

/// Parses a profile document (`file` names it in issues and EmulatorProfile::source). Issues: ParseError (wrong JSON
/// types, missing or unknown fields), ValidationFailed (an invalid id or name, a template that does not parse, an
/// environment variable outside the allow-list, a relative executable path, a negative timeout, a second profile with
/// the same id; a choice for a system name that is no folder name, an empty label, a malformed profile id; a program
/// without files, a file name with a folder, an append-file name that is no plain file name, a line with a line break,
/// a second program with the same id), Unsupported (a schemaVersion other than 1 and 2, checked first). "programs" in a
/// document of schemaVersion 1 is an unknown field.
[[nodiscard]] ProfileDocument parseProfileDocument(const nlohmann::json& document, std::string_view file,
                                                   ProfileOrigin origin);

/// Reads (core::readJsonFile: comments allowed, size-limited) and parses `file`; a file that cannot be read or parsed
/// is one issue (NotFound when it is missing).
[[nodiscard]] ProfileDocument loadProfileDocument(const std::filesystem::path& file, ProfileOrigin origin);

/// The profiles in effect.
struct EmulatorProfiles {
    /// The shipped profiles in their order, each replaced in place by the user's profile with the same id, then the
    /// user's other profiles in their order.
    std::vector<EmulatorProfile> profiles;
    /// The shipped document's choices, each system's replaced by the user's choice for it.
    SystemChoices systemChoices;
    /// The shipped programs in their order, each replaced in place by the user's program with the same id, then the
    /// user's other programs in their order.
    std::vector<ProgramProfile> programs;
    /// The shipped document's issues, then the user's. A user profile left out for an error does not replace the
    /// shipped one with its id, nor does a user choice left out replace the shipped choice of its system.
    std::vector<ProfileIssue> issues;

    [[nodiscard]] const EmulatorProfile* find(std::string_view id) const noexcept;
    /// The profiles that serve `system`, in order.
    [[nodiscard]] std::vector<const EmulatorProfile*> serving(std::string_view system) const;
    /// Retroverse's choice for `system` (emulators.json's "systemChoices"), or null.
    [[nodiscard]] const LaunchChoice* systemChoice(std::string_view system) const noexcept;
    /// The program `executable` is: the first of the user's programs whose files match its file name, else the first
    /// shipped one; null when none does.
    [[nodiscard]] const ProgramProfile* programFor(const std::filesystem::path& executable) const;
};

/// Merges two documents as EmulatorProfiles describes.
[[nodiscard]] EmulatorProfiles mergeProfileDocuments(ProfileDocument shipped, ProfileDocument user);

/// Loads the shipped document (its absence is an issue) and the user's (optional: its absence is none) and merges
/// them. Issues are also logged as warnings.
[[nodiscard]] EmulatorProfiles loadEmulatorProfiles(const std::filesystem::path& shippedFile,
                                                    const std::filesystem::path& userFile);
/// loadEmulatorProfiles() with <asset root>/launcher/emulators.json and <user data>/config/emulators.json.
[[nodiscard]] EmulatorProfiles loadEmulatorProfiles(const core::Paths& paths);

/// The rule that found an executable.
enum class LocationRule : std::uint8_t {
    AppPaths, ///< ExecutableSpec::appPathsName
    Path,     ///< one of ExecutableSpec::paths
};

/// "App Paths", "path".
[[nodiscard]] std::string_view toString(LocationRule rule) noexcept;

/// An executable found for a profile, and how (retroverse-cli reports which rule and entry matched).
struct LocatedExecutable {
    std::filesystem::path path;
    LocationRule rule = LocationRule::Path;
    /// The App Paths name, or the path as the profile writes it.
    std::string entry;

    friend bool operator==(const LocatedExecutable&, const LocatedExecutable&) = default;
};

/// Finds the executable of `spec` on the machine `probe` describes: App Paths (an absolute registered path to an
/// existing file), then each path in order, expanded with `environment` ('~' through ISystemProbe::homeFolder), the
/// first that names an existing file (ISystemProbe::fileExists), or, with wildcards in its last element, the first file
/// that matches (ISystemProbe::filesMatching). Error (NotFound): the message lists what was tried and why each failed
/// (not registered, missing, the home folder or an environment variable unknown, not an absolute path).
[[nodiscard]] Expected<LocatedExecutable> locateExecutable(const ExecutableSpec& spec, const core::ISystemProbe& probe,
                                                           const EnvironmentLookup& environment);

} // namespace rv::launcher
