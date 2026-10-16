# I2 step 5: emulator profiles, argument templates, launch plans and the automation guard

Step 5 of [I2](../roadmap.md) (milestone M1). It implements the profile half of
[ADR-0008](../decisions/ADR-0008-esde-catalog.md) §8 in `rv_launcher` (namespace `rv::launcher`, still on `rv_core`
alone). The ES-DE command translation of step 10 calls into these types.

## What was built

| File | Contents |
|---|---|
| `src/launcher/include/rv/launcher/arg_template.hpp` | Retroverse's own argument templates. Each element expands to exactly one argument and is never split again. Provides the tokens, the variable scopes, the environment allow-list and a check mode that needs no path values. |
| `src/launcher/include/rv/launcher/emulator_profile.hpp` | Emulator profiles in JSON with comments: parsing with JSON-pointer issues, shipped and user documents merged by id, and executables located through `core::ISystemProbe`. |
| `src/launcher/include/rv/launcher/launch_plan.hpp` | A neutral `LaunchTarget` plus a profile becomes a `LaunchRequest` or a `LaunchBlocker`. `isRefusedProgram()` names the command interpreters and launchers that are never started. |
| `src/launcher/include/rv/launcher/launch_guard.hpp` | The automation guard (`RV_LAUNCH_ALLOWLIST`): `LaunchGuard`, `GuardVerdict`, and `checkLaunchGuard()`, the call a runner makes. |
| `src/launcher/include/rv/launcher/environment.hpp` | New: `processEnvironmentVariable()`, which reads the live environment (unset, empty and set are told apart). |
| `assets/launcher/emulators.json` | The shipped defaults: one RetroArch profile with 50 systems. |
| `src/platform_win/src/win32_process_runner.cpp` | `Win32ProcessRunner::start` calls the guard before it creates anything. |
| `tests/unit/launcher/test_{arg_template,emulator_profile,launch_plan,launch_guard}.cpp` | 45 test cases. |

The rest of `rv_launcher` is unchanged. `launch_session.hpp` and `process_runner.hpp` only gained documentation about
the guard. `FailureReason` keeps its values, so the catalog's `PlaySession` mapping is untouched.

## The profile format

`<asset root>/launcher/emulators.json` holds the shipped profiles. `<user data>/config/emulators.json` holds the
user's, which is optional. Both are JSON with comments, `schemaVersion` 1, and a `profiles` array. Call
`loadEmulatorProfiles(paths)` (or `loadEmulatorProfiles(shippedFile, userFile)`) to load and merge them:

- the shipped profiles keep their order;
- a user profile with the same id replaces the whole shipped profile in place (there is no field-level merge);
- the user's other profiles are appended after them;
- every problem is a `ProfileIssue`, logged as a warning. It carries the file, the profile id when known, and the error.
  Its context is `<file>:<JSON pointer>`.

Errors are handled at three levels:

- **A broken profile** is left out, and the others still load.
- **A broken user file** (missing version, bad JSON) is left out entirely, and the shipped profiles stay.
- **A broken user profile** does not replace the shipped one with the same id.

The user file itself is never written. Choices made through the CLI go to the database (ADR-0008 §6).

```jsonc
{
  "schemaVersion": 1,
  "profiles": [
    {
      "id": "retroarch",                       // required: [a-z0-9][a-z0-9._-]*, at most 64 characters
      "displayName": "RetroArch",              // default: the id
      "executable": {                          // required: one path string, or this object
        "appPaths": "retroarch.exe",           //   App Paths (HKCU, else HKLM), tried first
        "paths": ["~\\AppData\\Roaming\\RetroArch\\retroarch.exe",
                  "{env:PROGRAMFILES}\\RetroArch\\retroarch.exe"]
      },
      "arguments": ["-L", "{var:core}", "{rom}"], // required ([] for none): one argument per element
      "workingDirectory": "{rom_dir}",         // default: the executable's folder (ADR-0008 §8)
      "environment": {"NAME": "value {system}", "REMOVED": null},
      "wait": "direct",                        // direct (default), tree, confirm
      "windowTimeoutSeconds": 30,              // 0: no window timeout (windowless console emulators); at most 86400
      "hideConsoleWindow": false,              // console programs without a window (ES-DE's %HIDEWINDOW%)
      "variables": {"cores": "{emulator_dir}\\cores"},
      "requiredFiles": ["{var:core}"],         // must exist, else "core not found"
      "systems": {
        "gb": {"variables": {"core": "{var:cores}\\gambatte_libretro.dll"}}
      },
      "verified": false                        // informational (ADR-0008 §8: never required to launch)
    }
  ]
}
```

A user profile for a standalone emulator, and an isolated RetroArch for the manual check of step 12. The isolation
arguments are I1's (`spikes/launch`), and the `.cfg` file must exist before the launch:

```jsonc
{
  "schemaVersion": 1,
  "profiles": [
    {"id": "mgba", "displayName": "mGBA", "executable": "{env:PROGRAMFILES}\\mGBA\\mGBA.exe",
     "arguments": ["-f", "{rom}"], "systems": {"gba": {}, "gb": {}, "gbc": {}}},
    {"id": "retroarch-isolated", "displayName": "RetroArch (isolated)",
     "executable": {"appPaths": "retroarch.exe"},
     "arguments": ["-L", "{var:core}", "--appendconfig={var:scratch}\\retroverse-isolated.cfg", "--max-frames=240",
                   "--sram-mode=noload-nosave", "--log-file={var:scratch}\\logs\\retroarch.log", "{rom}"],
     "variables": {"cores": "{emulator_dir}\\cores", "scratch": "C:\\path\\to\\retroverse\\build\\launch-scratch"},
     "requiredFiles": ["{var:core}"],
     "systems": {"gb": {"variables": {"core": "{var:cores}\\gambatte_libretro.dll"}}}}
  ]
}
```

These are the validation rules applied at load:

- Every template parses, and names only allowed environment variables.
- Environment names pass `validateOverride()`, and two names that differ only in case are refused.
- System names are one folder name each, compared exactly (as ES-DE does).
- Variable names are 1 to 64 of `[A-Za-z0-9_.-]`.
- Unknown fields are errors, at every level.
- An unknown top-level field is an issue, and the profiles still load. Step 10 adds the per-system choices of ADR-0008 §6
  to this format as a new top-level key.
- Executable paths are absolute once expanded. They may start with `~` (the user's home folder,
  `ISystemProbe::homeFolder`) or an `{env:NAME}` token, hold no other token, and have `*` or `?` only in their last
  element. A wildcard takes the first match in sorted order.

`locateExecutable()` tries the following, in order:

1. App Paths: an absolute registered path to a file that exists.
2. Each path, in order.

It reports the rule and the entry that matched (`LocatedExecutable`). When nothing matches, its error lists every
attempt and why it failed:

- the name is not registered;
- the file is missing;
- the home folder is unknown;
- a variable is not set;
- the path is not absolute.

### The shipped RetroArch profile

The profile is found through App Paths (the installer registers `retroarch.exe`), then through these paths:

- `~\AppData\Roaming\RetroArch`;
- `{env:SYSTEMDRIVE}\RetroArch-Win64`;
- `{env:PROGRAMFILES}\RetroArch-Win64`;
- `{env:PROGRAMFILES}\RetroArch`.

None of these is a machine path. The profile has these settings:

- **Command:** `-L {var:core} {rom}`.
- **Cores folder:** `cores = {emulator_dir}\cores`. A user profile changes it when `libretro_directory` points
  elsewhere.
- **Required file:** `requiredFiles = ["{var:core}"]`.
- **Wait strategy:** direct.
- **Window timeout:** 30 s.
- **Verified:** false.

The `core` of each of its 50 systems is ES-DE's default RetroArch core for that system. This is the first command of
`es_systems.xml`, as the survey found it (for example snes9x for snes and mame for mame, where the reference machine
uses bsnes and mame2003_plus through `<alternativeEmulator>`). The cores of systems the survey did not cover (gamegear,
genesis, ngp, pcenginecd, sega32x, wonderswan, wonderswancolor, atari800, gc, ps2) are the usual libretro defaults and
have not been checked against `es_systems.xml`.

## Tokens

| Token | Value |
|---|---|
| `{rom}` | the ROM as the launch names it (absolute), with `\` separators; a folder for games that are folders, without the trailing separator |
| `{rom_dir}` | the folder that holds the ROM |
| `{rom_name}` | the ROM's name without its last extension (`Mole Mania (USA)`) |
| `{rom_file}` | the ROM's name (`Mole Mania (USA).gb`) |
| `{rom_ext}` | the last extension with its dot (`.gb`), empty when there is none, so `{rom_name}{rom_ext}` = `{rom_file}` |
| `{emulator_dir}` | the folder of the located executable |
| `{system}` | the ES-DE system name |
| `{var:NAME}` | a variable: the launch's, else the system's, else the profile's |
| `{env:NAME}` | an allow-listed environment variable |

Text and tokens can be glued together (`-image={rom}`). Literal braces are written `{{` and `}}`.

**One element is one argument.** The expansion is never split again: a path with spaces, quotes, `&`, `%`, `^`, `;`,
`=`, non-ASCII text, or more than 260 characters stays one argument. An element that expands to nothing is an empty
argument. Results are UTF-8 and must pass `isValidUtf8`; a NUL character is an error.

**Variable values are templates.** A variable's value is expanded where it is used, and the variables it names are
looked up from the first scope again. A launch's `cores` therefore also changes the core path that the system writes as
`{var:cores}\...`. A variable may not refer to itself, directly or through others (the cycle is named), and nesting
stops at 32 levels.

**`{env:NAME}` is restricted to an allow-list of folders.** The allowed names are ALLUSERSPROFILE, APPDATA,
COMMONPROGRAMFILES, COMMONPROGRAMFILES(X86), COMMONPROGRAMW6432, HOMEDRIVE, HOMEPATH, LOCALAPPDATA, PROGRAMDATA,
PROGRAMFILES, PROGRAMFILES(X86), PROGRAMW6432, PUBLIC, SYSTEMDRIVE, SYSTEMROOT, TEMP, TMP, USERPROFILE and WINDIR.
Command lines are logged, so a template must never copy a token or a password out of the environment. Names ignore
case, and the lookup is asked for the allow-list's own spelling.

**Errors name what is wrong:**

- the token (`unknown token {romz}`);
- the variable (`unknown variable {var:core} (in {var:x})`);
- the environment variable that is not allowed or not set;
- the unmatched brace.

`checkTemplateElement()` validates a template without the path values. It is used to check configuration before the
executable is looked for.

## Launch plans

`planLaunch(target, profile, probe, options)` turns a `LaunchTarget` (`rom`, `system`, `variables`, `title`) into
either a `LaunchRequest` or a `LaunchBlocker`. The first check that fails decides the outcome. The order follows the
readiness order of ADR-0008 §8, which step 10 maps one to one:

| Order | Check | Blocker | Readiness (step 10) |
|---|---|---|---|
| 1 | the ROM path is absolute and exists (a file or a folder, through its `\\?\` form; `PlanOptions::checkRom`) | `RomMissing` | ROM missing |
| 2 | every template the launch uses (arguments, working directory, environment values, required files) checks for the target's system | `TemplateError`, or `VariableMissing` for an undefined `{var:}` or an unset `{env:}` | requires launcher configuration |
| 3 | `locateExecutable()` | `ExecutableNotFound` | emulator not found |
| 3 | not a command interpreter or launcher (`isRefusedProgram`: cmd, powershell, pwsh, wscript, cscript, mshta, rundll32, explorer, conhost, wt, wsl, bash, msiexec, regsvr32, by file name with the file-system casing) | `ProgramRefused` | requires launcher configuration |
| 4 | the values: arguments, working directory (must be absolute), environment, wait strategy, window timeout, `hideConsoleWindow`, label `<title> with <display name>` | `TemplateError` | requires launcher configuration |
| 5 | the required files exist (`ISystemProbe::fileExists`) | `RequiredFileMissing` | core not found |
| 6 | `validateLaunchRequest()` (a PE `.exe`/`.com`, no batch file, the working directory exists, the command line length) | `InvalidRequest` | requires launcher configuration |

These rules govern the result:

- **Configuration before location.** A profile that cannot work for a system needs configuration, whether or not its
  emulator is installed.
- **Unserved systems.** `profile.systems` lists the systems a profile is offered for. A launch for another system, such
  as a per-game choice, is planned with the launch's and the profile's variables, so a missing core is reported by name.
- **Ready plans.** A ready plan's request has already passed `validateLaunchRequest()`.
- **Reporting.** `LaunchPlan::executable` keeps the located executable even when a later check blocks the launch, so
  that `retroverse-cli profiles` can report it.
- **Default working directory.** An empty working directory in the request means the executable's folder.

## The automation guard

When `RV_LAUNCH_ALLOWLIST` is set (gate scripts, test harnesses, agents), only the executables it lists may start.
When it is unset, the guard does nothing.

**The variable's syntax:**

- Absolute paths, separated by `;`.
- Blanks and one pair of double quotes around an entry are ignored.
- Relative entries, device paths and streams are ignored and allow nothing (`ignoredEntries()`).
- A variable that is set but empty is active and allows nothing (fail closed).

A request is checked in this order, and the first rule that fails refuses it:

1. **The executable path.** It must be absolute: a drive or UNC path, or its `\\?\` form. Relative paths, `\\.\`,
   `\\?\GLOBALROOT`, `\??\` and alternate data streams are refused (`UnsupportedPath`).
2. **DuckStation.** Any `duckstation*.exe` is refused whether it is listed or not (`DuckStation`). The name is checked
   in two forms:
   - the name Windows opens, normalized as Windows does it (trailing dots and spaces dropped, `..` resolved);
   - the name of the file it resolves to, through `GetFinalPathNameByHandleW`. This expands 8.3 short names, follows
     symbolic links and junctions, and resolves subst and mapped drives.
3. **Resolution.** The file must exist and open for its attributes (`Unresolved`).
4. **RetroArch.** Any `retroarch*.exe` (by either name) needs isolation (`RetroArchNotIsolated`). It is isolated when
   two passes over the arguments both succeed:
   - **Every occurrence is usable.** Every argument that spells `--appendconfig` or abbreviates it, wherever it stands,
     is spelled out exactly: `--appendconfig=<files>`, or `--appendconfig` followed by `<files>`. Every `|`-separated
     file must be an absolute path to an existing file. Whichever occurrence RetroArch takes last therefore isolates.
     An abbreviation (which `getopt_long` may accept) or a missing value refuses.
   - **One occurrence is an option.** At least one of them is read as the option when the arguments are read as
     RetroArch's `getopt_long` reads them:
     - only options before the first `--` count;
     - non-options are permuted;
     - the value of each option that takes one is skipped, by RetroArch's own option table (43 options), so
       `-L --appendconfig` passes the core;
     - grouped short options are read too;
     - an option RetroArch does not know, or an ambiguous abbreviation, stops RetroArch with an error, and refuses here
       as well.

   A different case (`--APPENDCONFIG`) is an unknown option.
5. **The allow-list.** The request's resolved file must be the file a listed entry resolves to, compared with the
   file-system casing (`NotListed`).

The tests try to get past it in these ways:

- another case;
- an 8.3 short name, also in the list;
- relative paths;
- `..`;
- trailing dots and spaces;
- `/` separators;
- a junction;
- symbolic links, including one with an innocent name that leads to DuckStation, and a look-alike name;
- streams;
- device paths;
- the `\\?\` form.

**Limit:** a hard link or a copy under another name is not recognized by name. Only the list keeps it from starting.
The guard is a safety net for automation, not a security boundary.

### Where it applies, and what later steps do

- **`Win32ProcessRunner::start`** calls `checkLaunchGuard(request)` before anything is created. A refused request never
  exists, not even suspended, and the window is never handed over (`StartCallbacks::beforeResume` is not called).
  `LaunchSession` reports the refusal as `FailedToStart` with `FailureReason::StartFailed` and the guard's message. The
  error's context is `RV_LAUNCH_ALLOWLIST`.
- **`IProcessRunner`** documents this as the rule for every implementation that creates processes. Fake runners in
  unit tests create nothing, so the session tests do not depend on the environment.
- **A1 (app) and step 12 (`launch --game`)** start emulators only through `LaunchSession` on `Win32ProcessRunner`, so
  they need no call of their own.
- **Step 10 (`launch --dry-run`, `profiles`)** may call `checkLaunchGuard()` or `LaunchGuard::fromEnvironment().check()`
  to say what an automated run would refuse.
- **Any other code that starts a `LaunchRequest`** must call `checkLaunchGuard()` first.
- **Gate scripts and harnesses** that set the variable list the fake emulator
  (`build/<preset>/bin/rv_fake_emulator.exe`) and, for the manual RetroArch check, `retroarch.exe` with the isolated
  profile's `--appendconfig`.
- **Existing tests:** the runner tests in `tests/unit/platform_win` and the I1 spike tests start the fake emulator, so
  with the variable set they need it listed.

## Decisions taken within the step

**Templates and plans**

- Templates are JSON arrays of elements. A template string is never split; the splitting of ADR-0008 §8 is for ES-DE's
  commands.
- `{rom_ext}` keeps its dot. Path tokens use `\` and drop a trailing separator.
- Variables are scoped dynamically (the launch, then the system, then the profile, looked up again from the first
  scope inside values). They are never self-referential, and nest at most 32 levels.
- `{env:NAME}` is limited to an allow-list of folder variables, and only those. Executable paths also take a leading `~`
  and wildcards in their last element, as ES-DE's static paths do.
- `requiredFiles` is the generic form of "the core exists", so readiness can say "core not found" for any profile.
- Command interpreters and launchers are refused in profiles too (`isRefusedProgram`, shared with step 10). Quoting for
  `CommandLineToArgvW` is not quoting for `cmd.exe`, so a ROM named with `&` or `%` could otherwise become a command.

**Profiles**

- Profiles replace each other whole, by id. An `extends` field for derived profiles could follow if users ask for one.
- A profile does not refuse systems it does not list; the variables decide.

**The guard**

- The guard is enforced in the runner, not in `LaunchSession`. Every real process start is covered, including the
  spikes, and the session tests stay independent of the environment. `FailureReason` is unchanged, so a refusal is
  `StartFailed`.
- The allow-list matches resolved final paths, both for the request and for each entry. The name rules check both the
  requested and the resolved name.
- `retroarch*.exe`, not only `retroarch.exe`, needs isolation (`retroarch_angle.exe` and other builds).
- The isolation rule is stricter than "`--appendconfig` among the arguments":
  - every occurrence must be spelled out and name existing absolute files;
  - one occurrence must be an option under RetroArch's own option table;
  - unknown options refuse.

  A first version skipped the value of any option that might take one. That could hide a later, unusable
  `--appendconfig` behind a flag (`--appendconfig=good --max-frames-ss -L --appendconfig=bad`), so the reading now
  follows the table exactly and every occurrence is checked whatever its position.
- An empty `RV_LAUNCH_ALLOWLIST` is active (fail closed).

**Supporting API**

- `processEnvironmentVariable()` reads the live environment (`GetEnvironmentVariableW`). A variable set with
  `SetEnvironmentVariableW` is seen, which `_wgetenv` would miss.

## Tests

`build/msvc-x64-debug/bin/rv_tests.exe "launcher/*"` runs 78 test cases, 45 of them new, with about 8,000 assertions.

- **Templates:**
  - parsing and errors, with the token named;
  - every token, including folder ROMs, names without an extension and non-ASCII names;
  - variable scopes, cycles and depth;
  - the environment allow-list;
  - UTF-8 and NUL checks;
  - hostile ROM paths (spaces, `'`, `"`, `&`, `%`, `^`, `;`, `=`, non-ASCII, UNC, more than 300 characters) staying one
    element each, round-tripped through `buildCommandLine` and `CommandLineToArgvW`.
- **Profiles:**
  - every field and its default;
  - 34 field errors, each with its file, JSON pointer and profile id;
  - ids and required fields;
  - document errors (version first, unknown fields, duplicates, a broken profile among good ones);
  - the shipped/user merge, both through files with comments and through `core::Paths`;
  - the shipped defaults (no machine paths, a core for every system);
  - executable location on `FakeSystemProbe`: App Paths in HKCU before HKLM, a stale registration, `~`, `{env:}`, `..`,
    wildcards, and errors that name every attempt.
- **Plans:**
  - a RetroArch request that passes `validateLaunchRequest` with a program stub;
  - real hostile ROM files, including one past 260 characters, round-tripped through `CommandLineToArgvW`;
  - every blocker and its order (configuration before a missing emulator);
  - `isRefusedProgram`;
  - the shipped profile on a fake machine, located through App Paths and through `~`;
  - an end-to-end run: a planned request for a ROM path of more than 260 characters reaches `rv_fake_emulator`
    argument for argument through `Win32ProcessRunner`, in the emulator's own folder.
- **Guard:**
  - every rule;
  - every bypass attempt listed above;
  - the environment (unset, set, empty, non-ASCII entries);
  - `processEnvironmentVariable`;
  - the runner refusing an unlisted program before the hand-over, then starting it once it is listed.

## To fold into the shared documentation at the merge

- **`CLAUDE.md`:**
  - add `rv_launcher`'s profiles to the layout notes (`assets/launcher/emulators.json`,
    `<user data>/config/emulators.json`);
  - add `RV_LAUNCH_ALLOWLIST` to the safety rules: automated runs that may launch set it, DuckStation never runs, and
    RetroArch runs only with `--appendconfig`.
- **`docs/architecture.md`:** extend the `launcher` row with the argument templates, profiles, plans and the guard, and
  the `platform_win` row with the runner applying the guard.
- **`docs/roadmap.md`:** mark step 5 done.
- **`BACKLOG.md`:** candidates:
  - profile inheritance (`extends`);
  - a `startupGraceSeconds` per profile;
  - hard links in the guard (compare file ids with known emulator installations).
