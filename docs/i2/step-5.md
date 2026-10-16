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
| `tests/unit/launcher/test_{arg_template,emulator_profile,launch_plan,launch_guard}.cpp` | 52 test cases. |

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

**The work is bounded.** Nesting alone does not bound it: when each variable uses the one before it twice, the work
doubles with every level, 2^31 expansions within 32 levels (about an hour and 4 GiB of text for one check). Two limits
apply to each element, also in `checkTemplateElement()`, and are checked as the expansion goes:

- `kMaxTemplateTokens` (1,024): the tokens expanded, those in the values of its variables included. The error names
  the token and the variable it is in (`... more than 1024 tokens ... at {var:v0} (in {var:v1})`).
- `kMaxExpandedBytes` (98,298, three bytes per UTF-16 unit of the longest command line): the expansion's length. No
  longer text fits a command line, a path or an environment value.

Both are `ValidationFailed` errors (`TemplateError` blockers in a plan) and stop the expansion within milliseconds.

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
| 5 | `validateLaunchRequest()` (a PE `.exe`/`.com`, no batch file, the working directory exists, the command line length) | `InvalidRequest` | requires launcher configuration |
| 6 | the required files exist (`ISystemProbe::fileExists`) | `RequiredFileMissing` | core not found |

These rules govern the result:

- **Configuration before location.** A profile that cannot work for a system needs configuration, whether or not its
  emulator is installed.
- **Configuration before the core.** A request Windows cannot start (a batch file, a missing working directory) is
  reported before a missing required file, so a game never shows "core not found" first and "requires launcher
  configuration" once the core is installed. What depends on the executable can only be told once it is found, so it
  follows "emulator not found".
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
3. **Resolution.** The file must exist, open for its attributes and tell its file id (`Unresolved`).
4. **RetroArch.** Any `retroarch*.exe` (by either name) needs isolation (`RetroArchNotIsolated`; the message names the
   argument by its index and the reason, never an argument's value). The arguments are read as RetroArch 1.21.0 reads
   them, conservatively. Its Windows builds parse with libretro-common's compat getopt, not GNU `getopt_long`
   (`retroarch.c`, `compat_getopt.c` and the installed 1.21.0 binary were checked): every option is read wherever it
   stands; long options match by exact name, no abbreviations, the name being the first run of characters other than
   `=` after `--` (so `--=appendconfig` is `--appendconfig`), the value after the first `=` of the argument, else the
   next argument; short options come from its option string `hs:fvVS:A:U:DN:d:e:P:R:M:HC:F:L:r:c:`, grouped; `--` is
   no option; an unknown option stops RetroArch; the last `--appendconfig` read wins. It is isolated when all of these
   hold:
   - **The command line reaches it as written.** RetroArch's `main()` reads the ANSI code page's form of its command
     line (its manifest declares no UTF-8 code page), which Windows makes with best-fit characters: a fullwidth
     quotation mark (U+FF02) becomes `"` and splits an argument in two, so a ROM named `a＂ --appendconfig=bad ＂.gb`
     would hand RetroArch an `--appendconfig` of its own. The executable's path and every argument must come back
     unchanged from that code page (a Windows-1252 machine passes `Pokémon`, refuses `日本`; a UTF-8 code page passes
     everything).
   - **Every occurrence is spelled out.** Every argument that may select `--appendconfig` in any reading (its name
     after `--`, or after `--` and any `=`, is `appendconfig`, an abbreviation of it or a name holding it), wherever
     it stands, is `--appendconfig=<files>` or `--appendconfig` followed by `<files>`, with usable files.
   - **The reading is sure.** Read as RetroArch reads them, the arguments hold no option it does not know (from its
     option table of 44 long options; `--`, abbreviations and options of older versions such as `--bsvplay`
     included), no lone `-`, no grouped short options running into a long one (`-v-appendconfig`), no option
     without its value (RetroArch would take one from the arguments it moved behind: refused rather than modelled),
     and at least one `--appendconfig`, each with usable files. A value is never taken for an option
     (`-L --appendconfig` passes the core), and a value glued to an option without one is ignored (`--verbose=x`).
   - **The appended files are files RetroArch opens.** RetroArch only logs a file it cannot append and then runs on
     its own configuration, so each `|`-separated file must be an absolute drive or UNC path (no `\\?\` form, device
     or stream), in ASCII (RetroArch opens the files named by its ANSI arguments as UTF-8 text; any text when the
     code page is UTF-8), of at most 259 characters as written and once normalized (RetroArch is not long-path aware),
     and an existing file; the whole value at most 4,095 bytes (RetroArch keeps no more of it).

   A different case (`--APPENDCONFIG`) is an unknown option.
5. **The allow-list.** The request's resolved file must be the file a listed entry resolves to: the same final path,
   compared with the file-system casing, and the same file, by volume and file id (`FILE_ID_INFO`, else the older
   64-bit index). A folder with case-sensitive names (NTFS, per folder) holds `emu.exe` and `EMU.EXE` as two files
   whose paths fold alike; the identity keeps them apart. An entry that cannot be opened allows nothing (`NotListed`).

The tests try to get past it in these ways:

- another case;
- a folder with case-sensitive names, holding two files whose names differ only in case;
- an 8.3 short name, also in the list;
- relative paths;
- `..`;
- trailing dots and spaces;
- `/` separators;
- a junction;
- symbolic links, including one with an innocent name that leads to DuckStation, and a look-alike name;
- streams;
- device paths;
- the `\\?\` form;
- for RetroArch: `--=appendconfig`, `--==appendconfig=x`, abbreviations, `--`, values taken for options, grouped short
  options, an `--appendconfig` that is not the last one read, fullwidth characters, appended files with non-ASCII or
  long paths, a value longer than RetroArch keeps.

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
- An element expands at most 1,024 tokens and 98,298 bytes. A token budget, not a cache of expanded values: it also
  bounds the variables whose values are empty (no text grows, the time still doubles) and the parsing of long values,
  and no legitimate profile comes near it.
- The plan validates the request before it checks the required files (review of the step): readiness puts "requires
  launcher configuration" before "core not found".
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
- The allow-list matches resolved final paths and file ids, both for the request and for each entry; an entry that
  cannot be opened matches nothing (its folded path alone would match another file in a folder with case-sensitive
  names). The name rules check both the requested and the resolved name.
- `retroarch*.exe`, not only `retroarch.exe`, needs isolation (`retroarch_angle.exe` and other builds).
- The isolation rule is stricter than "`--appendconfig` among the arguments":
  - every occurrence must be spelled out and name files RetroArch opens;
  - the arguments are read as RetroArch 1.21.0 reads them, by its own option table and option string, and every
    `--appendconfig` read must be usable;
  - unknown options refuse.

  A first version skipped the value of any option that might take one. That could hide a later, unusable
  `--appendconfig` behind a flag (`--appendconfig=good --max-frames-ss -L --appendconfig=bad`), so the reading
  follows the table exactly and every occurrence is checked whatever its position.
- The reading models libretro-common's compat getopt, which RetroArch's Windows builds compile (its configure check
  for `getopt_long` fails with MinGW, and the installed 1.21.0 binary holds none of the MinGW `getopt` messages), not
  GNU `getopt_long` (review of the step: `--=appendconfig` passed an earlier version). Where GNU `getopt_long` would
  read the same arguments otherwise (abbreviations, `--`), it would stop with an error, which the reading refuses
  anyway, so a build with either parser is covered. The table holds 1.21.0's options with every build option; a
  newer RetroArch's new options are refused until the table learns them.
- RetroArch reads the ANSI code page's form of its command line and opens the files of its arguments as UTF-8 text
  without long path support (`main()`, `fopen_utf8`, its manifest): the guard refuses what would reach it as other
  text and appended files it could not open. The guard's process has the system's code page, as RetroArch has:
  Retroverse's manifests declare no active code page (a UTF-8 one there would hide the conversion from the guard).
- An empty `RV_LAUNCH_ALLOWLIST` is active (fail closed).

**Supporting API**

- `processEnvironmentVariable()` reads the live environment (`GetEnvironmentVariableW`). A variable set with
  `SetEnvironmentVariableW` is seen, which `_wgetenv` would miss.

## Tests

`build/msvc-x64-debug/bin/rv_tests.exe "launcher/*"` runs 85 test cases, 52 of them new, with about 8,300 assertions.

- **Templates:**
  - parsing and errors, with the token named;
  - every token, including folder ROMs, names without an extension and non-ASCII names;
  - variable scopes, cycles and depth;
  - the limits: a 32-level chain of variables that each use the one before twice, with text and empty, expanded and
    checked, stops within milliseconds; path tokens count; the byte limit for variables, literal text, the ROM path and
    environment values;
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
  - every blocker and its order (configuration before a missing emulator; a batch file or a missing working
    directory before a missing core);
  - a chain of variables used over and over, in the profile or in the launch, blocks the plan at once;
  - `isRefusedProgram`;
  - the shipped profile on a fake machine, located through App Paths and through `~`;
  - an end-to-end run: a planned request for a ROM path of more than 260 characters reaches `rv_fake_emulator`
    argument for argument through `Win32ProcessRunner`, in the emulator's own folder.
- **Guard:**
  - every rule;
  - every bypass attempt listed above (the folder with case-sensitive names is made with `FileCaseSensitiveInfo`, and
    the test is skipped where Windows cannot);
  - RetroArch's ANSI command line, against an independent check of the code page (`WC_NO_BEST_FIT_CHARS`), and the
    files it opens;
  - the refusal messages, which name the argument and never its value;
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
  - hard links in the guard (compare file ids with known emulator installations);
  - best-fit argument mangling for every emulator, not only under the guard: an emulator whose `main()` reads the ANSI
    code page receives a ROM path that the code page does not hold as other text, and a fullwidth quotation mark in
    it splits the argument ("WorstFit"). Plans could refuse or warn for such paths, or prefer emulators known to read
    UTF-16 (their manifest, `wmain`);
  - the guard's RetroArch option table follows RetroArch releases (1.21.0 today).
