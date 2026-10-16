# I2 step 2: the ES-DE configuration

Roadmap row: I2 step 2 (wave 1, needs 0b and 1). Decision: [ADR-0008](../decisions/ADR-0008-esde-catalog.md) §3, §6,
§7, §8. This page is folded into the shared documentation at the wave merge.

## What was built

`rv_esde` (the step-2 files of the skeleton) and two commands of `retroverse-cli`:

| File | What it does |
|---|---|
| `rv/esde/discovery.hpp` | `discoverEsde()`: the install folder (explicit, else the ES-DE uninstall entry through `core::ISystemProbe`, its DisplayVersion as the version) and the ES-DE home (explicit, else `portable.txt`, else the user's home folder), each with its source (`InstallSource`, `HomeSource`) and the issues met; `profileOf()` (a `catalog::LibraryProfile`); `bundledResource()` (ES-DE's resource lookup); `checkFolder()` (a folder there, missing, unreachable or not listable); library.json: `readLibraryFile()`, `libraryFileText()`, `writeLibraryFile()` (`ExistingFile::Keep`: a rename that replaces nothing, so a file there or created meanwhile is kept, Conflict) |
| `rv/esde/settings.hpp` | `readSettings()` / `readSettingsFile()`: the whitelisted keys of es_settings.xml (`settingKeys()`: ROMDirectory, MediaDirectory, ShowHiddenFiles, ShowHiddenGames, ParseGamelistOnly, LegacyGamelistFileLocation, AlternativeEmulatorPerGame, UIMode) with ES-DE's defaults; `resolveRomRoot()`, `resolveMediaRoot()` (`ResolvedRoot`: the folder, its source, the setting as written, ES-DE's own text of it); `esdeGenericText()`, `esdeReplace()`, `folderFromEsdeText()` (ES-DE's path texts) |
| `rv/esde/systems.hpp` | `loadSystems()` / `loadSystemsFiles()`: es_systems.xml, custom first (`SystemsConfiguration`: one `SystemDefinition` per name, the `fallbacks` ES-DE tries when a definition finds no game, `<loadExclusive/>`); `systemRomFolder()`; `systemInfos()` (the domain's `catalog::SystemInfo`, the ROM folder's availability checked); `gamelistFile()`; `readOnlyFolders()` (the folders a library only reads, for the data-directory guard) |
| `rv/esde/find_rules.hpp` | `loadFindRules()` / `loadFindRulesFiles()`: es_find_rules.xml, custom first (`FindRules`: emulators and cores, typed rules with their entries in order). Parsing only: step 10 evaluates them |
| `rv/esde/command_template.hpp` | `splitCommandLine()` (exactly shell32's CommandLineToArgvW), `parseCommandTemplate()` (elements, tokens, the program's index), `scanCommandTokens()`, `censusKey()` |
| `tools/cli/src/cmd_discover.cpp`, `cmd_library.cpp` | `retroverse-cli discover`, `library show`, `library create`; what both share (the folder options, how folders, roots and issues print) is in the private header `tools/cli/src/esde_text.hpp`, which no source list names |
| `tools/cli/src/command.cpp`, `commands.hpp`, `rv/cli/command.hpp` (step 0a's dispatcher) | the data-directory guard before the log file: `Command::checksDataDirectory`, `checkDataDirectory()`, `startFileLog()` (see "The user data directory and the log file" below) |

Tests: `tests/unit/esde/test_command_template.cpp`, `test_esde_settings.cpp`, `test_esde_discovery.cpp`,
`test_esde_systems.cpp`, the hidden `test_esde_real.cpp` (`[.real-library]`), and `tests/unit/cli/test_cli_discover.cpp`.
New fixtures (each with a README stating its expectations): `tests/fixtures/esde/portable`, `custom-dirs`,
`custom-systems`; `standard` is used read-only.

## ES-DE 3.4.1's behaviour, as verified in its source

Read from the tag `v3.4.1` of `gitlab.com/es-de/emulationstation-de` (read-only web access): `es-app/src/main.cpp`,
`es-core/src/utils/FileSystemUtil.cpp`, `es-core/src/utils/StringUtil.cpp`, `es-core/src/Settings.cpp`,
`es-core/src/resources/ResourceManager.cpp`, `es-app/src/SystemData.cpp`, `es-app/src/FileData.cpp`,
`es-app/src/UIModeController.cpp`.

**Home folder, ES-DE home, `~`.**
- ES-DE's home folder (`Utils::FileSystem::getHomePath`) is the folder `setHomePath()` was given (by `--home` or by
  portable mode), else `%HOMEDRIVE%` + `/` + `%HOMEPATH%` (both must be set), else ES-DE's current working folder.
- The ES-DE home (`getAppDataDirectory`, settings/, gamelists/, custom_systems/) is always `<home folder>/ES-DE`. So
  `~`, which is the home folder, is the ES-DE home's parent, as ADR-0008 §8 says since its step-1 amendment.
  Retroverse's probe falls back to `%USERPROFILE%` instead of a working folder it cannot know.
- `~` is expanded by `expandHomePath()`, which replaces **every** `~` of the text, not only a leading one. This holds
  in ROMDirectory, MediaDirectory, es_systems.xml paths, staticpath find rules, `%STARTDIR%` values and whole
  commands. Retroverse replaces every `~` too and reports one past the start of ROMDirectory or
  MediaDirectory (`root-tilde-inside`); a system's `<path>` is replaced without a report.

**`--home`.** `--home <folder>` must name an existing folder (or a drive) that is not a file, else ES-DE exits. It
names the home folder, not the ES-DE home: ES-DE then uses `<folder>\ES-DE`. It wins over portable mode.
`retroverse-cli --home` takes it the same way.

**Portable mode** (`parseArguments` in main.cpp). ES-DE looks for `portable.txt` in the folder of ES-DE.exe and reads
its first line with `std::getline` on a stream opened in text mode:
- reading stops at a Ctrl+Z, `\r\n` reads as `\n`, and a byte order mark is not skipped;
- an empty first line makes the home folder the ES-DE.exe folder;
- otherwise the line is appended to that folder (`<exe folder>/<line>`, `/` turned into `\`), even when the line
  holds an absolute path;
- the result must exist (or be a drive) and must not be a file, else ES-DE prints an error and ignores `portable.txt`.

**Settings** (`Settings::loadFile`).
- `es_settings.xml` (`<ES-DE home>/settings/`) is parsed with pugixml's `load_file` without `parse_fragment`. pugixml
  still accepts the file's many root-less elements.
- Only the top-level `<bool>`, `<int>`, `<float>` and `<string>` elements are read, each into its own map. A key
  written under another element type is therefore not that setting, and when a key appears twice the later element
  wins.
- A boolean is pugixml's `as_bool()`: true when the value starts with `1`, `t`, `T`, `y` or `Y`, false otherwise or
  when the attribute is missing.
- A missing file leaves ES-DE's defaults. A file that fails to parse is logged and the defaults are used, and ES-DE
  later rewrites the file.
- A legacy `<ES-DE home>/es_settings.xml` is moved into `settings/` and read when `settings/es_settings.xml` does not
  exist.
- The defaults of the keys Retroverse uses: ROMDirectory and MediaDirectory empty, ShowHiddenFiles, ShowHiddenGames and
  AlternativeEmulatorPerGame true, ParseGamelistOnly and LegacyGamelistFileLocation false, UIMode `full`. Any UIMode
  but `full`, `kid` or `kiosk` becomes `full` (`UIModeController`).

**ROM and media roots** (`FileData::getROMDirectory`, `getMediaDirectory`).
- An empty ROMDirectory gives `<home folder>/ROMs/`.
- Otherwise ROMDirectory gets `~` replaced, then a `\` appended when it does not end with one, then `%ESPATH%`
  replaced by the folder of ES-DE.exe in its generic form (`/` separators).
- An empty MediaDirectory gives `<ES-DE home>/downloaded_media/`.
- Otherwise MediaDirectory gets `~` replaced, then `%ESPATH%`, then a `/` appended.
- Both separators are accepted, since ES-DE's generic paths turn `\` into `/`.

**Resources** (`ResourceManager::getResourcePath`). A bundled resource (`:/systems/windows/es_systems.xml`, the find
rules, the MAME lists) is taken from `<ES-DE home>/resources/` when the file exists there, else from
`<exe folder>/resources/`.
- A missing es_systems.xml makes ES-DE stop ("Program resource missing").
- A missing es_find_rules.xml only leaves the custom rules.

**Systems** (`SystemData::loadConfig`, `getConfigPath`).
- Files and their order:
  - `<ES-DE home>/custom_systems/es_systems.xml` is read first, when it exists, then the bundled file.
  - A top-level `<loadExclusive>` in the custom file leaves the bundled file unread. In the bundled file it is ignored
    with a warning.
  - A file that cannot be parsed, or has no `<systemList>`, makes ES-DE refuse to start.
- Duplicate names:
  - A `<system>` whose name matches a system already **loaded** is skipped.
  - Loaded means the folder exists and holds at least one game. So a custom definition replaces the bundled one only
    when it loads; otherwise ES-DE goes on to the next definition of the name (`SystemsConfiguration::fallbacks` keeps
    those for the ROM scan).
- Name, full name and path:
  - `<name>` and `<fullname>` have their line feeds removed, not replaced with spaces.
  - `<path>` has `%ROMPATH%` replaced by the ROM root text, `\` turned into `/`, `//` collapsed (repeatedly, which
    also breaks a network path) and `~` expanded. `%ESPATH%` is **not** expanded in `<path>`.
- Extensions and platforms are split by `readList` on space, tab, CR, LF and comma. Extensions are kept verbatim
  (`CUE` matches nothing). Platforms are lower-cased first.
- `<theme>` falls back to the name when it is missing or empty.
- Validation:
  - A system without a name is skipped.
  - So is one without a full name, path, extension or command, after the duplicate and folder checks.
  - Placeholder commands (`PLACEHOLDER %ROM%`) are only skipped by `--create-system-dirs`. When loading, they fail as
    "emulator not found" when launched.
- Command labels:
  - One command may go without a label.
  - When several commands are given, an unlabelled one stops the reading: only the commands before it are kept.
  - A labelled command after an unlabelled first one stops it too: only the first command is kept.
  - A repeated label is ignored.
  - Labels compare exactly.
- `<systemsortname>` only orders ES-DE's system list.

**Find rules** (`FindRules::loadFindRules`, `FileData::findEmulator`).
- Files and duplicates:
  - The custom `es_find_rules.xml` is read first, then the bundled one.
  - The first definition of an emulator or core name wins, in one file too. A file that cannot be parsed is skipped.
  - Emulators without a name, rules without a type and types ES-DE does not read on Windows are skipped with a
    warning. Emulators may have `winregistrypath`, `winregistryvalue`, `systempath` and `staticpath`; cores only
    `corepath`.
- Entries are the text of `<entry>`, not trimmed.
- Evaluation, for step 10:
  - Order: every winregistrypath, then winregistryvalue, systempath, staticpath.
  - App Paths (winregistrypath) are read under HKCU, and under HKLM only when the HKCU key cannot be opened.
  - A winregistryvalue entry `key\value|suffix` gets the suffix appended verbatim.
  - systempath uses `SearchPathW` with no path and `.exe` as the default extension. The result is the found folder
    plus the entry as written.
  - A staticpath expands `~`, `%ESPATH%` and **`%ROMPATH%`**.
  - A `*` is allowed in the last element only. The pattern is a regular expression (`*` becomes `.*`, `.` stays a
    wildcard character), matched case-sensitively against the generic paths sorted ordinally; the first match wins.
  - `|command` replaces the program.

**Command tokens on Windows** (`FileData::launchGame`).
- Flags, removed wherever they are (each followed by a left trim): `%RUNINBACKGROUND%`, `%HIDEWINDOW%`,
  `%ESCAPESPECIALS%`.
- `"%CORE_` loses its quote, then `~` is expanded in the whole command.
- Find rules: `%PRECOMMAND_X%`, then `%EMULATOR_X%` (named up to the next `%`), `%CORE_X%` with `%EMUPATH%`.
- `%STARTDIR%` must be followed by `=` and at least two characters. A value starting with `"` runs to the next `"`
  (no escapes) and ES-DE removes the one character after that quote with it; otherwise it runs to the next space (a
  tab is part of it), removed with that space. Nothing is trimmed after it. It accepts `~`, `%EMUDIR%`, `%GAMEDIR%`
  and `%GAMEENTRYDIR%`. ES-DE creates the folder.
- `%INJECT%=`: a file of at most 4,096 bytes, relative to the game's folder. Its value may use `%BASENAME%` or be
  exactly `%ROM%`.
- Values: `%ROM%`, `%BASENAME%`, `%FILENAME%`, `%ROMRAW%`, `%ROMPATH%`, then `%ESPATH%`, `%EMUDIR%`, `%GAMEDIR%`,
  `%GAMEDIRRAW%`.
- Anything else is left as written: `%ROMRAWWIN%` and `%ENABLESHORTCUTS%` (other platforms), the Android tokens, and
  `%RPCS3_GAMEID%`.
- ES-DE then runs `CreateProcessW` with no application name.

**CommandLineToArgvW** (shell32, checked on this machine through P/Invoke and by the tests).
- The algorithm is Wine's documented one, including the "three quotes" rule inside a quoted part (not the 2008 C
  runtime rule).
- An unquoted first element ends at **any** code unit from U+0001 to U+0020. A scan of every BMP code unit showed
  it, not only the `isspace` set. That one character is consumed.
- The elements after the first are separated by space and tab only.
- An empty command line returns the program's own path.

## Retroverse's choices within the step

**ES-DE behaviour followed:**
- Every `~` is replaced, with an issue when it is not the first character.
- The bundled resources of the ES-DE home are preferred.
- Custom files come first and `<loadExclusive/>` is honoured.
- ES-DE's label rules apply, and extensions are kept verbatim.
- portable.txt's first line is read as ES-DE reads it, byte order mark and Ctrl+Z included.
- A portable home ES-DE rejects falls back to the default home.

**Deliberate differences, each reported as an issue or documented in the headers:**
- es_settings.xml:
  - When the file exists but cannot be parsed, loading fails with ParseError; ES-DE would run on its defaults.
    Retroverse refuses to guess the roots, so the import keeps what it has.
  - A missing file gives the defaults, as in ES-DE.
- Roots that cannot be resolved are reported (`root-unresolved`), not guessed:
  - a relative ROMDirectory or MediaDirectory, which ES-DE resolves against its own working folder;
  - `%ESPATH%` without a known install folder.
- A network-share root (`\\server\share`) is kept and reported (`root-network-share`); ES-DE 3.4.1's generic paths
  collapse its leading `\\`, so ES-DE itself cannot read it. A home or install folder on a share keeps its leading
  pair too (`esdeGenericText()`), so `~` and `%ESPATH%` still give absolute folders.
- `.` and `..` are resolved lexically in the folders kept, as Windows resolves them.
- A system name that is no valid folder name (`SystemName`) is skipped (`system-name-invalid`).
- An unparseable custom `es_find_rules.xml` is skipped as ES-DE skips it, but reported as an error issue.
- `parseCommandTemplate()` reads what leads a command as `launchGame()` reads it, one element each: a flag where the
  text starts with it (`%HIDEWINDOW%%EMULATOR_X%` too), then the blanks `std::isspace` knows (a line feed too);
  `%STARTDIR%=` with its value by ES-DE's rule above (`%STARTDIR%="C:\Games\" %EMULATOR_X%` gives `%STARTDIR%=C:\Games\`
  and the program `%EMULATOR_X%`; an unquoted value takes a tab and what follows it). The program-name rule applies to
  what follows: the command line ES-DE runs once it removed them, so `%STARTDIR%="C:\Games dir" "C:\Emu dir\emu.exe"
  %ROM%` gives three elements. Retroverse also skips blanks after the start folder, which ES-DE leaves at the start of
  the command line it runs. A quoted value without its closing quote (ES-DE refuses the command) leaves no program
  (`programIndex == elements.size()`). For any command that starts with its program, the elements are exactly
  `splitCommandLine()`'s. Flags and start folders elsewhere in a command stay elements (step 10 removes them).
- A ROM root override is no text of ES-DE's: `systemRomFolder()` replaces the `~` of a system's path first and puts
  the override in after it, so a `~` of the override's own (`D:\EMULAT~1\ROMs`) stays. ROMDirectory keeps ES-DE's
  order, in which its `~` is replaced twice.
- The folder options of `discover` and `library create` (`--home`, `--install`, `--rom-root`, `--media-root`) take a
  bare drive (`E:`) as its root, as ES-DE does (its `--home E:` makes `E:\ES-DE` the ES-DE home), never as that
  drive's current folder.
- The version comes from the uninstall entry's DisplayVersion when that entry describes the install folder; otherwise
  it is unknown. ES-DE.exe's version resource is not read, since that would need a new probe function.
- `checkFolder()` reads the folder's attributes only; whether it can be listed is the ROM scan's.

**Token kinds** (`CommandTokenKind`):

| Kind | Tokens |
|---|---|
| Variable | `%ROM%`, `%ROMRAW%`, `%ROMPATH%`, `%BASENAME%`, `%FILENAME%`, `%GAMEDIR%`, `%GAMEDIRRAW%`, `%GAMEENTRYDIR%`, `%EMUDIR%`, `%ESPATH%`, `%EMUPATH%` |
| Flag | `%HIDEWINDOW%`, `%ESCAPESPECIALS%`, `%RUNINBACKGROUND%` |
| Assignment | `%STARTDIR%=`, `%INJECT%=` (the value is the rest of the element) |
| Emulator, Core, PreCommand | `%EMULATOR_X%`, `%CORE_X%`, `%PRECOMMAND_X%` (the name runs to the next `%`) |
| HomeFolder | `~` |
| Malformed | `%STARTDIR%` or `%INJECT%` without `=` |
| Unknown | any other `%NAME%` (a letter or `_`, then letters, digits, `_-!.()`), with its `=` when one follows |

Known tokens are matched exactly and case-sensitively, as ES-DE matches them, so neither a stray `%` nor an unknown
name before one hides it: `50%-%ROM%` and `x%y%ROM%` hold `%ROM%`.

**Issue codes** (`catalog::Issue::code`):

| Code | Severity |
|---|---|
| `home-unknown`, `home-missing` | error |
| `install-not-found`, `install-incomplete`, `portable-home-invalid`, `portable-unreadable` | warning |
| `settings-missing`, `settings-legacy-file` | info |
| `setting-malformed`, `setting-wrong-type` | warning |
| `setting-repeated` | info |
| `root-unresolved` | error |
| `root-tilde-inside`, `root-network-share` | warning |
| `system-skipped`, `system-name-invalid`, `command-label-missing`, `command-label-duplicate`, `rom-folder-unresolved` | error |
| `system-repeated`, `load-exclusive-ignored` | warning |
| `system-replaced`, `extension-without-dot`, `platform-missing` | info |
| `find-rules-unreadable` | error |
| `find-rules-missing`, `find-rule-skipped`, `find-rule-repeated` | warning |
| `find-rule-replaced` | info |
| `user-data-inside-library` (discover) | error |

The settings reader extracts only the whitelisted keys, so their values are the only ones an issue may name. The
scraper credentials and the UI mode's passkey are never read, logged or returned. The tests check this on three
fixtures with fake credentials: settings, roots, issues, every log line at trace level, the CLI's outputs, its log
file and library.json.

## library.json

`<user data>/config/library.json`, JSON with comments, written atomically by `library create`:

```jsonc
// The library Retroverse reads (ADR-0008 §6), written by "retroverse-cli library create". ...
{
  "schemaVersion": 1,
  "id": "esde",
  "adapter": "esde",
  "esdeHome": "C:\\Users\\me\\ES-DE",
  "esdeInstall": "C:\\Program Files\\ES-DE",
  "romRoot": null,
  "mediaRoot": null
}
```

The fields:
- `esdeHome` is the ES-DE home itself; `~` is its parent.
- `esdeInstall`, `romRoot` and `mediaRoot` may be null or absent. `romRoot` and `mediaRoot` replace ROMDirectory and
  MediaDirectory.

What `readLibraryFile()` refuses:
- unknown keys, a missing or invalid id, adapter or esdeHome, and relative or empty folders (InvalidArgument);
- another schemaVersion (Unsupported).

The database is the authority on the id (ADR-0008 §4). From step 4 on, a mismatch must be refused.

## CLI

```text
retroverse-cli [global options] discover [--home <dir>] [--install <dir>] [--json] [--output <file>]
retroverse-cli [global options] library [show] [--json] [--output <file>]
retroverse-cli [global options] library create [--id <slug>] [--home <dir>] [--install <dir>]
                                               [--rom-root <dir>] [--media-root <dir>] [--force] [--json]
                                               [--output <file>]
```

**`discover`** finds ES-DE as ES-DE finds itself. `--home` is ES-DE's own `--home` (the ES-DE home is `<dir>\ES-DE`);
`--install` defaults to the uninstall entry.
- It prints, with their sources:
  - the ES-DE home and the home folder (`~`);
  - the install folder (the uninstall key when it came from there) and the version;
  - portable mode;
  - the settings file and the eight values Retroverse uses;
  - the ROM and media roots (`override`, `setting` with the value as written, or `default`);
  - the find rules (emulators, cores, entries);
  - the systems: configured, custom, commands, and those with a ROM folder (name, full name, folder);
  - the gamelists ES-DE would read (and those of folders no system names, folder names compared as the file system
    compares them);
  - the issues.
- `--json` prints the same as one object. Its keys are `esdeHome`, `homeFolder`, `install`, `version`, `portable`,
  `settings`, `romRoot`, `mediaRoot`, `findRules`, `systems` (with `withRomFolder`), `gamelists` and `issues`; what is
  not known is null (the version, the uninstall key, a root that cannot be resolved).
- A `--home` naming the ES-DE home itself (a folder holding `settings`) is the likely mistake: the `home-missing` issue
  says so, and `library create` repeats it.
- Settings or systems that cannot be read are reported as `settings-unreadable` or `systems-unreadable` (error), with
  the reason; discover still prints what it found.
- It writes nothing but its log file: the tests compare the fixture's files (content hashes) and folders before and
  after. The log file is written only when the user data directory lies outside the folders discover found
  (`esde::readOnlyFolders()`); otherwise the error `user-data-inside-library` says so, and nothing is written.
- Exit codes: 0; 1 when an error issue was met (no ES-DE home, a configuration that cannot be read, a user data
  directory inside the library); 2 for invalid options; 3 when `--output` cannot be written.

**`library show`** (also plain `library`) prints the profile and the roots it gives with ES-DE's settings.
- Exit codes: 0; 1 when there is no library.json; 2 when the user data directory lies inside the library's folders; 3
  when library.json cannot be read or is invalid.

**`library create`** discovers ES-DE (with `--home` and `--install` as discover takes them), then records the result.
- It requires an existing ES-DE home, and ROM and media roots it can resolve: from the settings, else `--rom-root` and
  `--media-root` must give them (the data directory could not be kept out of an unknown root).
- It prints discovery's warnings (an install folder not found, an ignored `portable.txt`) on stderr.
- It refuses a user data directory inside the folders the library reads (`esde::readOnlyFolders()` through
  `checkDataDirectory()`, which is `core::checkUserDataDirectory`): the ES-DE home, the install folder, the effective
  ROM and media roots, and the ROM folder of every system outside them (an absolute `<path>` or one using `~`, which
  the ROM scan reads too). It refuses before anything is written, the log file included.
- It writes library.json atomically and never replaces one without `--force`: not when it is there (nor when it cannot
  tell whether one exists), and not when another `library create` writes one meanwhile (the final rename replaces
  nothing: `ExistingFile::Keep`). Then it prints the library as `show` does.
- Exit codes: 0; 2 for invalid options and for what they must change (no ES-DE home, a root that cannot be resolved, a
  data directory inside the library, library.json already there, an invalid id); 3 when it cannot be written.

**The user data directory and the log file.** Retroverse writes nothing in the folders of the library it reads
(ADR-0008 §6, §7), and that includes its log file. The dispatcher (step 0a's `command.cpp`) therefore logs to stderr
only until the user data directory has passed the guard, then `startFileLog()` opens
`<user data>/logs/retroverse-cli.log`:
- For `discover` and `library` (`Command::checksDataDirectory`), whose options can name the library, the command
  checks the folders it found and starts the log file itself. A usage error, found before any folder is known, leaves
  no log file (the process test `unit.cmake/cli_process` exempts these two from its log-file check).
- For every other command the dispatcher, before the command runs, reads `<user data>/config/library.json` when there
  is one and refuses a user data directory inside that library's folders (`esde::readOnlyFolders(profile)`: its
  settings and systems read, what cannot be read left out): the message on stderr, exit 2, nothing written. Without a
  library.json there is nothing to keep out of.

## Real library (`scripts/real-library.ps1`, with ES-DE closed)

`./scripts/real-library.ps1 -EsdeHome C:\Users\sinhu\ES-DE -EsdeInstall E:\Programmi\ES-DE -RomRoot I:\Giochi\Emulatori
-MediaRoot E:\Programmi\ES-DE_media\downloaded_media` (Release, the script's default; also run with
`-Preset msvc-x64-debug`)

Both `[real-library]` tests passed (2,709 assertions) in both builds, and the snapshots showed 0 differences among
542,082 entries each time.

What discovery found, with no options:
- The install folder through `HKLM\SOFTWARE\WOW6432Node\…\Uninstall\ES-DE`, version 3.4.1.
- The ES-DE home under the user's home folder.
- The ROM root and media root from ROMDirectory and MediaDirectory.

What was read:
- 195 systems, 191 of them with a ROM folder, and 889 commands.
- Find rules: 119 emulators and 1 core, 444 entries.
- Every command splits exactly as CommandLineToArgvW and parses into the same elements.

Census of the command tokens (occurrences, commands):

| Token | Occurrences | Commands |
|---|---|---|
| `%EMULATOR_X%` | 882 | 882 |
| `%ROM%` | 799 | 799 |
| `%CORE_X%` | 413 | 413 |
| `%STARTDIR%=` | 176 | 176 |
| `%EMUDIR%` | 158 | 158 |
| `%GAMEDIR%` | 116 | 110 |
| `%HIDEWINDOW%` | 115 | 115 |
| `%ROMPATH%` | 110 | 107 |
| `%BASENAME%` | 98 | 89 |
| `%INJECT%=` | 41 | 41 |
| `%ESCAPESPECIALS%` | 34 | 34 |
| `%GAMEDIRRAW%` | 25 | 22 |
| `%ROMRAW%` | 18 | 18 |
| `%RUNINBACKGROUND%` | 13 | 13 |
| `%FILENAME%` | 3 | 3 |
| `%RPCS3_GAMEID%` | 2 | 2 |

Four commands are placeholders.

Seven commands have a program other than one `%EMULATOR_X%` element:
- the four placeholders: androidapps, androidgames, lutris, xboxone;
- three hand edits: gc "Dolphin" and "Dolphin (Standalone)", ps2 "PCSX2 (Standalone)".

The user also added a staticpath entry to the bundled find rules: PCSX2's first one is `K:\Giochi\Emulatori\PS2\
pcsx2-qt.exe`, ahead of the two upstream `pcsx2-qt*.exe` wildcards.

The hidden test checks every figure above, not only prints them: the counts, the census table, the seven commands and
the PCSX2 entry. A change to the token classification or the program detection fails the gate run.

## For the later steps

- **Step 7 (ROM scan):**
  - A system ES-DE loads is the first definition whose folder exists and lists a game; then come its `fallbacks`.
  - With ShowHiddenFiles off, ES-DE also skips a hidden ROM folder.
  - `gamelistFile()` follows LegacyGamelistFileLocation.
  - The MAME lists are bundled resources: use `bundledResource()`.
- **Step 10 (launch):**
  - The program is `CommandTemplate::programIndex`.
  - Find rules are evaluated as described above.
  - The tokens to expand follow the token table; `~` is a token.
- **Steps 4 and 11:** library.json's id against the database's.

## Suggested ADR-0008 amendments

The code follows ES-DE 3.4.1's source and the guard below; the ADR's sentences could say so at the merge.

**§8, `~`.** §8 says `~` is ES-DE's home folder: "`%USERPROFILE%`, the `--home` folder, or the folder of `ES-DE.exe`
when portable". ES-DE 3.4.1's source is more precise on three points:
- the default is `%HOMEDRIVE%%HOMEPATH%`; Retroverse's probe falls back to `%USERPROFILE%` where ES-DE would use its
  own working folder;
- in portable mode it is the ES-DE.exe folder only for an empty `portable.txt`; otherwise it is that folder joined with
  the file's first line;
- a bare drive given as `--home` (`E:`) is that drive's root: the ES-DE home is `E:\ES-DE`.

**§8, find rules.** The sentence on static paths ("`~` ..., `%ESPATH%` the install folder; a wildcard only in the last
component with matches taken in sorted order") leaves out two facts step 10 needs:
- static paths expand `~`, `%ESPATH%` and `%ROMPATH%` (ROMDirectory as ES-DE's `getROMDirectory()` gives it);
- a `*` in the last element is matched as ES-DE's regular expression over the generic path (`*` becomes `.*`, `.`
  stays a wildcard character, `()[]` escaped), case-sensitively, the first match in ordinal order winning.

**§6 and §7, the data-directory guard.** The folders the user data directory may not lie in are the ES-DE home, the
install folder, the ROM root and the media root, and also the ROM folder of every system outside the ROM root (an
absolute `<path>`, or one using `~`): the ROM scan reads those too (`esde::readOnlyFolders()`). The guard also comes
before the log file: retroverse-cli logs to stderr only until the user data directory has passed it.

## For the merge

This step changed three files of step 0a's CLI skeleton besides its own commands, to put the guard before the log file
(see "The user data directory and the log file"):
- `tools/cli/include/rv/cli/command.hpp`: `Command::checksDataDirectory` (a new last field, `true` in the rows of
  `discover` and `library`), and `run()`'s description;
- `tools/cli/src/command.cpp` and `commands.hpp`: `checkDataDirectory()`, `startFileLog()`, the dispatcher's guard on
  library.json's folders;
- `tests/unit/cli/test_cli_commands.cpp` and `tests/cmake/test_cli_process.cmake`: a usage error of `discover` or
  `library` leaves no log file.

A command another step implements needs nothing new: the dispatcher still starts its log file before it runs, once
the guard has passed.
