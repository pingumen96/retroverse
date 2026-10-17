# I2 step 10: ES-DE launch translation and readiness

Roadmap row: I2 step 10 (wave 2, needs 2, 3, 4 and 5). Decision: [ADR-0008](../decisions/ADR-0008-esde-catalog.md) §6
and §8. This page is folded into the shared documentation at the wave merge.

Outcome: an ES-DE command of a system becomes a `launcher::LaunchRequest` for a game, or the blockers that say why it
cannot start, with ES-DE's own reading of the command, its find rules and its choice of command, on the machine
`core::ISystemProbe` describes. Readiness follows ADR-0008 §8's precedence. `retroverse-cli profiles` shows, for each
system, the command or profile its games launch with, why, its readiness and the find rule that found the program;
`retroverse-cli launch --dry-run` shows the request a game would start, argument for argument. On the reference
machine, the 48 systems that have games come out as the survey expected (41 ready with RetroArch found through App
Paths, ps3 and windows requiring launcher configuration, atari800, gc, ps2, switch and trs-80 with their emulator not
found), and the five launches ES-DE logged there are reproduced argument for argument, with 0 differences in the
library's snapshots.

## What was built

| File | What it does |
|---|---|
| `rv/catalog/launch_resolution.hpp` (`rv_catalog`) | The domain side: `LaunchReadiness` (the six states in their precedence, `readinessOf()`), `ReadinessBlocker`, `FoundLocation`, `LaunchTranslation`; the choice (`ChoiceSource`, `LaunchChoices`, `ChosenLaunch`, `chooseLaunch()`); `LaunchGame`; the port `ICommandTranslator`; `LaunchContext`, `LaunchResolution`, `resolveLaunch()`; `translateProfile()` (a Retroverse profile planned with `launcher::planLaunch()`, its blockers mapped to readiness) |
| `rv/esde/launch_translation.hpp` (`rv_esde`) | The ES-DE side: `TranslationContext`; `findEmulator()` and `findCore()` (the find rules evaluated on the probe, `FindResult` with the rule and entry that matched and every attempt); `translateCommand()`; `EsdeCommandTranslator` (the catalog's port over a `SystemsConfiguration`); `listedPath()` (a relative path spelled as the folder lists it) |
| `rv/launcher/emulator_profile.hpp` (`rv_launcher`, step 5's loader) | `LaunchChoiceKind`, `LaunchChoice`, `SystemChoices`; emulators.json's new top-level key `"systemChoices"` in `ProfileDocument`, merged per system in `EmulatorProfiles` (`systemChoice()`) |
| `tools/cli/src/cmd_profiles.cpp`, `cmd_launch.cpp` | `retroverse-cli profiles`, `retroverse-cli launch --dry-run`; what both share is in the private header `tools/cli/src/launch_text.hpp`, which no source list names |

Tests: `tests/unit/esde/test_find_rule_evaluation.cpp`, `test_launch_translation.cpp`, `test_launch_fixtures.cpp` (with
the helper `launch_test_machine.hpp`), the hidden `test_launch_real.cpp` (`[.real-library]`);
`tests/unit/catalog/test_launch_resolution.cpp`; `tests/unit/launcher/test_system_choices.cpp`;
`tests/unit/cli/test_cli_launch.cpp`. No new fixture: `standard` and `custom-systems` are used read-only (variants are
made in their materialized copies).

## How a launch is resolved

```cpp
// The library as `profiles` and `launch` read it (tools/cli/src/launch_text.hpp, loadLaunchLibrary()).
const rv::esde::TranslationContext context{profile, romRoot, &findRules, &probe};
const rv::esde::EsdeCommandTranslator translator(context, systems);         // systems: esde::loadSystems()
const rv::launcher::EmulatorProfiles profiles = rv::launcher::loadEmulatorProfiles(paths);
const rv::catalog::LaunchContext launch{&translator, &profiles, &probe, nullptr};

rv::catalog::LaunchChoices choices;
choices.alternativeEmulatorPerGame = settings.alternativeEmulatorPerGame;  // es_settings.xml
choices.systemAlternativeEmulator = systemInfo.alternativeEmulator;       // the gamelist's <alternativeEmulator>
choices.gameAltEmulator = entry.altEmulator;                              // the game's <altemulator>
if (const auto* choice = profiles.systemChoice(systemInfo.name.str())) {  // emulators.json's "systemChoices"
    choices.system = *choice;
}
// choices.game: Retroverse's choice for the game, from the database (step 12).

rv::catalog::LaunchGame game;
game.relativePath = entry.key.relativePath();  // as listed on disk
game.availability = entry.availability;        // Missing: "ROM missing"; SourceUnavailable: "source unavailable"
game.title = entry.title;

const rv::catalog::LaunchResolution resolution = rv::catalog::resolveLaunch(systemInfo, &game, choices, launch);
if (const auto request = resolution.request()) {
    // Ready: start it through a LaunchSession on Win32ProcessRunner (step 12, A1), which applies the guard.
} else {
    // resolution.readiness(), resolution.translation.blockers say why.
}
```

`resolveLaunch(systemInfo, nullptr, ...)` gives a system's readiness: what does not depend on a game (the program, the
core, the tokens); an %INJECT% file and a `%STARTDIR%=%GAMEDIR%` folder are each game's (a note says so), and no
request is built. A game shares its system's readiness unless its own file, its `<altemulator>` or Retroverse's choice
for it differ, which is what step 13 needs for the card.

`LaunchTranslation::request` is the request as far as it could be built, also when blockers remain (a dry run shows its
arguments, a token that blocks the launch kept in them as written); `LaunchResolution::request()` gives it only when
ready, and a ready request has passed `launcher::validateLaunchRequest()`.

## The translation (ADR-0008 §8)

**Splitting and expansion.** `parseCommandTemplate()` (step 2) splits the command as CommandLineToArgvW splits a command
line and reads what leads it (flags, `%STARTDIR%=`) as ES-DE does. Each element's tokens are then replaced by their
values, in place, and a value is never split or expanded again: a ROM named `Game %BASENAME% 50%.gb` stays one argument
holding `%BASENAME%`, where ES-DE's sequential string replacements would replace it inside the path.

**The program**, decided on the unexpanded command: exactly one `%EMULATOR_X%` element (the find rules of X), or a
literal absolute path whose only tokens are `~` and `%ESPATH%` (the three hand-edited commands of the reference machine
are of this kind). Anything else is "requires launcher configuration" (`program-not-allowed`), and so is a program
whose file name `launcher::isRefusedProgram()` names (cmd, powershell, explorer, ...; `program-refused`), even when a
find rule found it. The request must pass `validateLaunchRequest()` (`invalid-request`: a batch file, a file that is no
PE program, a working folder that is not there, a command line too long).

**Tokens.** The values, for a game `<ROM folder>\<relative path>`:

| Token | Value | ES-DE 3.4.1 |
|---|---|---|
| `%ROM%`, `%ROMRAW%` | the game's path: ES-DE's text of the system's ROM folder (ROMDirectory as configured, the system's `<path>` with `%ROMPATH%` replaced, `\` turned into `/`, repeated separators collapsed, `~` replaced) and the relative path as listed on disk, `\` between the elements | `getEscapedPath(mPath)` quotes `%ROM%` when it holds a space; one argument either way |
| `%ROMPATH%` | the ROM root, ES-DE's text of it without its trailing separator | `getEscapedPath(getROMDirectory())` |
| `%BASENAME%` | the game's name up to its last `.`; a folder game's whole name | `getStem()`, which keeps a directory's name whole |
| `%FILENAME%` | `%BASENAME%` and the last extension, `.` when there is none (`runme` gives `runme.`, a folder `Game.ps3` gives `Game.ps3.ps3`) | `baseName + getExtension(romPath)`, mirrored; ES-DE's own `%FILENAME%` also ends with a quote when the path holds a space, which CommandLineToArgvW then reads back to the same argument in every command in use |
| `%GAMEDIR%`, `%GAMEDIRRAW%` | the folder holding the game | `%GAMEDIR%` quoted when it holds a space |
| `%EMUDIR%` | the program's folder | |
| `%ESPATH%` | the install folder (`install-unknown` when it is not known) | never quoted |
| `~` | ES-DE's home folder, the ES-DE home's parent, in ES-DE's text (`/` separators) | `expandHomePath()` on the whole command first |
| `%CORE_X%\<file>` | `<corepath entry>\<file>`: the entry with `~`, `%EMUPATH%` (the program's folder) and `%ESPATH%` replaced, then `\` and the text after the token's separator, the first entry whose file exists (`core-not-found` otherwise) | the core file is what follows the token and one more character, to the next space or quote |
| `%STARTDIR%=<folder>` | the working folder, with `~`, `%EMUDIR%` and `%GAMEDIR%` replaced; without it, the program's folder | ES-DE creates a missing folder; Retroverse writes nothing (`start-folder-missing`) |
| `%HIDEWINDOW%` | `LaunchRequest::hideConsoleWindow` | removed wherever it is |
| `%INJECT%=<file>` | dropped (the element, or its value glued to a prefix) when the file is missing, not a file, empty or over 4,096 bytes once its line ends are removed (as ES-DE reads it: lines joined without their `\r` and `\n`, up to a Ctrl+Z), since ES-DE then injects nothing; a file ES-DE would inject is "requires launcher configuration" (`inject-file`). The value has `~` and `%BASENAME%` replaced, `%ROM%` alone is the game's file, and a value without a drive (`X:\`) lies in the game's folder | |

Anything else is "requires launcher configuration", the token named once (`token-unsupported`): `%RUNINBACKGROUND%`,
`%ESCAPESPECIALS%`, `%PRECOMMAND_X%`, `%GAMEENTRYDIR%` and `%EMUPATH%` in a command, unknown `%X%` text
(`%RPCS3_GAMEID%`, `%APPDATA%`, the tokens of ES-DE's other platforms). Also `token-malformed` (`%STARTDIR%` or
`%INJECT%` without `=`, a `%STARTDIR%=` naming no folder), `program-repeated` and `core-repeated` (a second
`%EMULATOR_X%` or `%CORE_X%`, which ES-DE leaves unexpanded), `start-folder-repeated`, `start-folder-glued` (a
`%STARTDIR%=` inside an argument), `start-folder-relative`, `command-placeholder`, `program-missing` (a quoted start
folder never closed), `find-rules-missing` (no rule defines X), `rom-root-unknown`.

**Find rules** (`findEmulator()`, `findCore()`), on the probe, in ES-DE's order whatever the document's:

1. every `winregistrypath` entry: App Paths, the key's default value under HKCU, and under HKLM only when the HKCU key
   does not exist (a HKCU key without a value hides HKLM's, as in ES-DE); the value as written, quotes included (ES-DE
   keeps them, and a quoted value names no file for it either);
2. every `winregistryvalue` entry, `<key>\<value>|<suffix>`: the key and value name read as ES-DE reads them (a path's
   parent and file name), HKCU then HKLM by the same rule, the suffix appended verbatim (`InstallLocation|mGBA.exe`
   relies on the value's trailing backslash, `Path|\dosbox-x.exe` brings its own);
3. every `systempath` entry: the ES-DE install folder, System32, the Windows folder, then PATH's absolute entries in
   order, `.exe` added to a name without an extension (SearchPathW's default extension); never Retroverse's folder or
   the current folder;
4. every `staticpath` entry: `~`, `%ESPATH%` and `%ROMPATH%` (ES-DE's `getROMDirectory()` text) replaced, `..`
   resolved; a `*` in the last element matched as ES-DE matches it, with `std::regex` (`*` becomes `.*`, `()[]`
   escaped, `.` and the rest keep their meaning) against the names of the folder's files as listed, case-sensitively,
   the first in ordinal order; a `|` replacement program makes the search undecided (`find-rule-undecided`, requires
   launcher configuration) once its path exists, and is skipped while it does not, as ES-DE skips it.

The first existing file wins; `FindResult::attempts` lists every attempt and why it failed, which `profiles` and
`launch --dry-run` print with "emulator not found" and "core not found". The custom `es_find_rules.xml` comes first
(`loadFindRules()`, step 2). An entry that needs `%ESPATH%` without a known install folder, `%ROMPATH%` without a
resolved ROM root, or a `systempath` search without the install folder (ES-DE's SearchPathW looks there first) makes the
search undecided rather than "not found".

**The choice of command** (`chooseLaunch()`), most specific first:

1. Retroverse's choice for the game (`LaunchChoices::game`, the database's from step 12);
2. the game's `<altemulator>`, while `AlternativeEmulatorPerGame` is on (off: a warning says it is not used);
3. Retroverse's choice for the system (emulators.json's `systemChoices`, later also the database's);
4. the gamelist's `<alternativeEmulator>`;
5. the system's first command.

A Retroverse choice is honoured or blocks, never skipped: a label the system no longer has is `choice-stale`, a profile
id that does not exist `profile-unknown` ("requires launcher configuration", naming it). ES-DE's own labels that name
no command fall back to the first command with ES-DE's warning, as ES-DE's do: an invalid `<alternativeEmulator>` (ES-DE
marks it `<INVALID>`) to the first command; an invalid game's `<altemulator>` to the first command too, never to the
gamelist's `<alternativeEmulator>`, which ES-DE does not look at then. Retroverse's choice for the system stands in for
ES-DE's default: when there is one, an invalid game's `<altemulator>` falls to it.

**Readiness** (`resolveLaunch()`): the system's folder unavailable, or the game's (`source-unavailable`); the game's
file missing (`rom-missing`); the choice's blocker; then the translation's blockers. The readiness is the first of
`LaunchReadiness`'s order among them: configuration before location (a token Retroverse does not run outranks a
missing emulator), and what depends on the program (refused, not a PE program) after "emulator not found" but before
"core not found", as step 5's plans order them. Profiles map `launcher::BlockerKind` one to one: template errors,
missing variables, refused programs and invalid requests require configuration (`profile-template`,
`profile-variable-missing`, `program-refused`, `invalid-request`), a missing executable is `emulator-not-found`, a
missing required file `core-not-found`.

## emulators.json: Retroverse's choice for a system

A new top-level key of step 5's format, in the shipped and the user's documents (the user's replaces the shipped one's
per system):

```jsonc
{
  "schemaVersion": 1,
  "profiles": [ /* ... */ ],
  "systemChoices": {
    "snes": {"command": "bsnes"},       // one of the system's ES-DE commands, by its label (compared exactly)
    "gb": {"profile": "retroarch"}      // one of the profiles, by its id
  }
}
```

Each choice is checked on its own (a system name that is no folder name, an empty label, a malformed profile id, an
unknown field, both or neither of the two keys are issues with their JSON pointer; the other choices are kept). Whether
the label or the profile exists is the resolution's to tell, at every refresh and launch. The shipped file sets none:
ES-DE's own choice applies until the user makes one.

## CLI

```text
retroverse-cli [global options] profiles [--system <name>] [--json] [--output <file>]
retroverse-cli [global options] launch --dry-run --system <name> --path <relative path> [--json] [--output <file>]
```

Both read the library `<user data>/config/library.json` records (exit 2 without one: run `library create` first), its
settings, systems, find rules and gamelists, and the emulator profiles, on this machine (`Win32SystemProbe`). They
start nothing and write nothing but the log file.

**`profiles`** lists the emulator profiles (where each executable was found, or why not), emulators.json's system
choices, then every configured system: the command or profile its games launch with and why ("the gamelist's
`<alternativeEmulator>`"), its readiness, the program and the core with the rule and entry that found them, the
blockers, the attempts behind "emulator not found" and "core not found", the notes; then each of its commands and each
profile serving it, with their own readiness, the chosen one marked `*` (so a system that cannot launch shows what
could: ps2's first command, for example). `--json` gives the same as one object: `library`, `profiles`,
`systemChoices`, `systems` (each with `name`, `fullName`, `romFolder`, `availability`, `alternativeEmulator`,
`systemChoice`, `chosen` {`kind`, `value`, `source`, `why`, `warnings`}, `readiness`, `program` and `core`
{`path`, `rule`, `entry`, `detail`}, `blockers` [{`readiness`, `code`, `message`}], `attempts`, `notes`, `commands`,
`profiles`) and `issues`. Exit codes: 0; 1 with `--system` when that system is not ready; 2 for invalid options, an
unknown system or no library.json; 3 when ES-DE's settings or systems cannot be read, or `--output` written.

**`launch --dry-run`** resolves one game, named by its system and its path relative to the system's ROM folder (game ids
come with the import, step 11): the path is resolved below the folder (`core::resolveRelativePath`, `..` refused) and
spelled as the folder lists it (`esde::listedPath`: what ES-DE's `%ROM%` ends with); its gamelist entry gives its title
and `<altemulator>`. It prints the game and its availability, the command or profile and why, the command as
es_systems.xml writes it, the readiness, the program, core, blockers and notes, then the request: argv (the program,
then each argument as the emulator receives it), the working folder, the environment overrides, the console window,
the command line (`launcher::commandLineOf`), and what the automation guard would say of it
(`LaunchGuard::fromEnvironment().check()`: inactive, allowed, or the refusal). `--json` gives `system`, `path`,
`listedPath`, `directory`, `availability`, `romPath`, `title`, `alternativeEmulator`, `chosen`, `command`, `readiness`,
`ready`, `program`, `core`, `blockers`, `attempts`, `notes`, `request` {`argv`, `workingDirectory`, `environment`,
`hideConsoleWindow`, `waitStrategy`, `label`}, `commandLine`, `guard` {`rule`, `allowed`, `message`} and
`started: false`. Command lines are logged at debug level only (ADR-0008 §9). Without `--dry-run` it answers "not
implemented yet (I2 step 12)" and exits 2. Exit codes: 0 ready; 1 not ready; 2 for invalid options, no `--dry-run`, an
unknown system, a path that is not below the system's ROM folder, no library.json; 3 as `profiles`.

## ES-DE 3.4.1's behaviour, as verified in its source

Read from the tag `v3.4.1` of `gitlab.com/es-de/emulationstation-de` (read-only web access): `es-app/src/FileData.cpp`
(`launchGame`, `findEmulator`) and `es-core/src/utils/FileSystemUtil.cpp`. Beyond step 2's notes:

- The values: `romPath = getEscapedPath(mPath)`, `baseName = getStem(mPath)`, `romRaw = getPreferredPath(mPath)`,
  `fileName = baseName + getExtension(romPath)`; on Windows `%ESPATH%` is the executable's folder with `\` (never
  quoted), `%EMUDIR%` and `%GAMEDIR%` the escaped parents of the emulator and of the game, `%GAMEDIRRAW%` the parent
  with `\`; `%ROMPATH%` is replaced before them.
- `getStem()` keeps a directory's whole name; `getExtension()` gives `.` for a name without one; `getEscapedPath()`
  quotes a path holding a space and turns `/` into `\`; `getParent()` cuts at the last `/` of the generic path.
- `%CORE_X%`: a quote before it is removed with its closing one; the core file is the text two characters after the
  token's closing `%`, to the next space (or the closing quote); each corepath entry is tried as
  `<entry>\<file>` with `~`, `%EMUPATH%` and `%ESPATH%` replaced, and the first that is a file or a link wins.
- `%INJECT%`: `=` must follow, the value is quoted or runs to the next space, and it is removed with the one space after
  it; `%BASENAME%` is replaced in it, a value of exactly `%ROM%` is the game's file, `\` becomes `/`, and a value
  without `X:/` lies in the game's folder; the file is read line by line, each line's `\r` removed, the lines joined;
  empty content and content over 4,096 bytes inject nothing.
- An invalid per-game `<altemulator>` (with `AlternativeEmulatorPerGame` on) logs a warning and the first command
  applies: the system's alternative emulator is the `else if` of the game's, never reached once a game's label is
  set; an invalid system alternative emulator logs a warning and the first command applies.
- A game that is a folder holding a regular file (or a link) of exactly its own name, compared case-sensitively, is
  launched through that file: `romPath` becomes the inner file, `baseName` the folder's name up to its first `.`, so
  `%ROM%`, `%BASENAME%`, `%FILENAME%`, `%GAMEDIR%`, `%GAMEDIRRAW%`, `%STARTDIR%=%GAMEDIR%` and the folder of a relative
  `%INJECT%` file follow it, while `romRaw` (`%ROMRAW%`, `%INJECT%=%ROM%`) stays the folder.
- `%CORE_X%`: when a corepath entry uses `%EMUPATH%` or `%ESPATH%` (each replaced once), every `/` of the core's path
  becomes `\`; a path holding a space goes through `getEscapedPath` (quoted, so its generic form with `\`); the core
  file ends at the next space after the token (the closing quote when a quote opens it), and without one ES-DE refuses
  the command ("invalid entry in systems configuration file"). A `<core>` without corepath entries is "missing core
  configuration", an `<emulator>` without entries of the four types `NO_RULES`.

## Decisions taken within the step

**ES-DE behaviour followed:** the find rules' order and HKCU/HKLM rule, registry values with their quotes, the suffix
appended verbatim, static paths with `%ROMPATH%`, wildcards with ES-DE's regular expression and case-sensitive names,
the `|` replacement only once its path exists, `%FILENAME%` and `%BASENAME%` as ES-DE builds them, `%INJECT%`'s content
rules, ES-DE's stale labels falling back to the first command, folder games launched through their inner file, core
paths normalized as ES-DE normalizes them.

**Deliberate differences, each documented in the headers:**

- Split, then expand (ADR-0008 §8): where ES-DE's own argv breaks (unquoted `%BASENAME%` with spaces, a ROMDirectory
  with a space inside the nested MAME commands), Retroverse's stays one argument per element.
- The executable search never looks in Retroverse's folder or the current folder (ADR-0008 §8); a `systempath` entry
  without an extension is found with `.exe` and the request names that file, where ES-DE runs `<folder>\<entry>` and
  lets CreateProcessW add `.exe` (no entry of the reference machine lacks one).
- The ES-DE install folder unknown: a search that would need it is undecided ("requires launcher configuration", give
  it to `library create --install`), never "not found".
- A static path's wildcard is matched against the files the probe lists (`ISystemProbe::filesMatching(folder, "*")`),
  where ES-DE lists folders too: a folder that sorts first and matches makes ES-DE's rule fail, not Retroverse's.
- A `%INJECT%` value glued to a prefix (`-id=%INJECT%=%BASENAME%.3dsen`) leaves the prefix as its own argument; ES-DE
  removes the value with the space after it, which would glue the prefix to the next argument (in the commands of
  ES-DE 3.4.1 the glued form is always last).
- `%EMUDIR%` and `%EMUPATH%` with a literal program are the program's folder (ES-DE has no emulator path then).
- `%STARTDIR%` folders are never created (`start-folder-missing`); a second `%STARTDIR%`, one glued inside an argument,
  and `%GAMEENTRYDIR%` require launcher configuration.
- Without `%STARTDIR%` the working folder is the emulator's (ADR-0008 §8; ES-DE leaves emulators in its own folder).
- A literal program path with a space but no quotes ends at the space (CommandLineToArgvW's program-name rule), where
  CreateProcessW would try the longer names too: "emulator not found" rather than a guess.
- Retroverse's stored choices never fall back to another command (a missing core where the chosen one was present would
  change the emulator behind the user's back).

**Within the CLI:** `profiles` lists every configured system (not only those with games: the ROM scan, step 7, knows
which have some); the dry run resolves whatever path it is given below the ROM folder, a file ES-DE would not list
included (`Loud Disc (Japan).CUE` resolves with psx's command), and takes its title and `<altemulator>` from the entry
ES-DE applies to the file as listed (`esde::entryForListed`).

## Tests

`build/msvc-x64-debug/bin/rv_tests.exe "[launch]"`: 29 test cases, about 2,000 assertions (Debug).

- **Find rules** on FakeSystemProbe: App Paths in HKCU before HKLM, a HKCU key without a value, a registered file gone,
  a quoted value; registry values with both suffix forms and a missing key; the executable search's folders in order,
  the default extension, nothing found in a ROM folder or the current folder, an unknown install folder; static paths
  with `~` (the ES-DE home's parent, not the ES-DE home), `%ESPATH%`, `..` and `%ROMPATH%`, wildcards (case, order,
  `.` as a regular expression's wildcard, literal parentheses, a wildcard in a folder), the `|` replacement; the rule
  types in ES-DE's order whatever the document's; corepath; the custom find rules first (custom-systems fixture).
- **Translation**: the token table; `%BASENAME%` and `%FILENAME%` for extensions, no extension, folder games and leading
  dots; the commands in use (the apple2 nested escaped quotes, `;` lists, `-image=%ROM%`, backslashes before quotes)
  against an independent oracle, ES-DE's own string expansion with its quirks parsed by the real CommandLineToArgvW;
  hostile names (`%`, `^`, `&`, `;`, `~`, `%BASENAME%` in a name, non-ASCII) staying one argument; the program rule,
  the refused shells (cmd.exe through its find rule, PowerShell.EXE), literal programs; every token that requires
  configuration, named once; `%STARTDIR%` and `%HIDEWINDOW%`; the `%INJECT%` cases (missing, empty, line ends only,
  4,097 bytes, 4,096 over many lines, usable, glued, absolute, `%ROM%`, a system without a game); cores and the
  readiness precedence; `listedPath()`.
- **Fixtures**: the standard fixture's commands and games as its README expects them, `AlternativeEmulatorPerGame` off,
  emulators.json's system choices (a profile, a label, stale ones); the custom-systems fixture's custom rules, wildcard,
  `|` replacement, custom core folder, placeholder, extensionless game and labels.
- **Resolution**: the readiness names and precedence, the choice order, stale labels both ways, availability, profiles
  through `planLaunch()`.
- **systemChoices**: valid choices, every invalid form with its pointer, the merge.
- **CLI**: `profiles --json` and `--system` on the standard fixture, `launch --dry-run` (argv, working folder, the path
  as listed, not ready, the guard with `RV_LAUNCH_ALLOWLIST` empty, listing the emulator, unset), usage errors, no
  library.json; the fixture's files unchanged.

### Real library (`scripts/real-library.ps1`, with ES-DE closed)

`./scripts/real-library.ps1 -EsdeHome C:\Users\sinhu\ES-DE -EsdeInstall E:\Programmi\ES-DE -RomRoot I:\Giochi\Emulatori
-MediaRoot E:\Programmi\ES-DE_media\downloaded_media -RetroArchExe E:\Programmi\Retroarch\retroarch.exe
-Filter '[real-library][launch]'` (the two tests of `test_launch_real.cpp`, through retroverse-cli with a library.json in
a temporary user data directory):

- `profiles --json`: the 48 systems with games as expected: 41 ready, each with RetroArch found by `winregistrypath
  "retroarch.exe"` (the path RV_REAL_RETROARCH_EXE names) and its core by `%EMUPATH%\cores`, apple2 ("MAME - Current"),
  mame ("MAME 2003-Plus") and snes ("bsnes") through their gamelists' `<alternativeEmulator>`, the others with their
  first command; ps3 and windows requiring launcher configuration (`program-refused`: cmd.exe, and
  `%ESCAPESPECIALS%`); atari800 ("Altirra (Standalone)"), gc ("Dolphin (Standalone)", the hand-edited `K:` path), ps2
  ("PCSX2 (Standalone)", likewise), switch and trs-80 with their emulator not found. Over all 195 configured systems:
  131 ready, 15 requiring configuration, 36 emulator not found, 9 core not found, 4 source unavailable.
- `launch --dry-run --json` on the five frozen launches (Star Fox and Batman Returns with bsnes, Kirby's Adventure with
  mesen, Destruction Derby 2 with mednafen_psx, Mole Mania with gambatte): each game selected by its logged path, ready,
  the path as listed equal to ES-DE's, and argv equal to CommandLineToArgvW of the logged command line, case-sensitively:
  0 differences.
- Snapshots: 0 differences among 542,082 entries.

## For the later steps

- **Step 12 (`launch --game`, `assign`, sessions):** fill `LaunchChoices::game` (and `LaunchChoices::system` from
  `assign`, before or instead of emulators.json's: the brief of step 12 decides) with a `launcher::LaunchChoice` from the
  database; resolve again right before the launch and start `LaunchResolution::request()` through `LaunchSession`; a
  `PlaySession` records `chosen.choice` (a profile id, or the ES-DE label). `launch --dry-run` keeps working beside
  `--game`.
- **Step 13 (the app):** compute each system's readiness once per refresh (`resolveLaunch(system, nullptr, ...)`) and a
  game's only where it can differ (its availability, `<altemulator>`, a per-game choice, an `%INJECT%` file).
- **Step 11 (import):** `SystemInfo::alternativeEmulator` and `CatalogEntry::altEmulator` are what the resolution reads;
  the CLI reads the gamelists itself until the database holds them.

## For the merge

- **`CLAUDE.md`:** the two commands (`retroverse-cli profiles`, `launch --dry-run`) in the CLI notes; emulators.json's
  `systemChoices`; real-library runs: `-Filter '[real-library][launch]'` and the git-ignored
  `tests/real_library/local/esde_logged_launches.json`.
- **`docs/architecture.md`:** the `catalog` row gains launch resolution (`ICommandTranslator`), the `esde` row the
  launch translation (find rules on the probe, `EsdeCommandTranslator`), the `launcher` row the per-system choices.
- **`docs/roadmap.md`:** step 10 done.
- **ADR-0008 §8, suggested amendments:** an `%INJECT%` file is dropped when its content is over 4,096 bytes once
  its line ends are removed (ES-DE measures the content, not the file); App Paths values are read with their quotes, as ES-DE
  reads them; an unknown install folder makes a `systempath` search undecided too (ES-DE's SearchPathW looks there
  first); a static path's `|` replacement requires configuration once its path exists.
- **`BACKLOG.md`:** candidates: `%GAMEENTRYDIR%` in `%STARTDIR%` (unused by ES-DE 3.4.1's commands); a suggestion for a
  system that cannot launch (the first ready command, SC-10) in `profiles` and the app.
