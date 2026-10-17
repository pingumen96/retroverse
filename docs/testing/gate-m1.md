# Gate M1: the ES-DE catalog and the launcher

Gate M1 closes milestone M1 (specification §22) and iteration I2: Retroverse reads an existing ES-DE collection
without moving anything, and a title selected in the quick catalog launches in its emulator and comes back. The
decisions it rests on are in [ADR-0008](../decisions/ADR-0008-esde-catalog.md) (with
[ADR-0009](../decisions/ADR-0009-quick-hash-evidence.md)), ADR-0005 (the product UI) and ADR-0006 (launch and return);
what each I2 step built and measured is in [docs/i2/](../i2/). This page says what the gate checks, how to run it, the
figures of its run on the reference machine, where each M1 requirement is proven, and the manual session the user signs.

**Status: gate passed, manual checklist pending** (October 17, 2026, reference machine: `./scripts/gate.ps1 -Gate M1` and
`./scripts/gate.ps1 -Gate M1 -RealLibrary`). M1 is done once the user has signed the manual checklist below.

## What the gate checks

`scripts/gate.ps1 -Gate M1` runs everything Gate A runs ([gate-a.md](gate-a.md): the build matrix without a warning,
the unit tests and the GPU checks of Debug and Release, golden G01) and adds:

- **The M1 checks on fixtures**, per Debug and Release:
  - *M1 unit tests*: the test cases of the unit run that belong to M1 (the modules `catalog`, `catalog_db`, `esde`,
    `cli`, `launcher`, `platform_win`, the test support, the core primitives of I2, the app's catalog worker, catalog
    sessions, launch flow and run report, the quick catalog's source and model, the input scripts, `retroverse-cli` as a
    process), counted from the unit run's JUnit report: none may fail;
  - *M1 GPU catalog and launch*: the `gpu.catalog: ...` cases (the standard fixture imported with `retroverse-cli`,
    searched, filtered and launched into the fake emulator from `retroverse.exe`, back with the same selection, search,
    filter and camera; a blocked game; the card of a blocked game whole; no `library.json`) and the `gpu.launch: ...`
    cases (the launch shell of I2 step A1: a return with no frame rendered meanwhile, nine failure paths, the panel's
    questions, the wait limit, Retroverse closed mid-game), counted from the GPU run;
  - *M1 CLI as a process*: the unit test `cli/process` (`tests/unit/cli/test_cli_m1_flow.cpp`), every command a
    `retroverse-cli.exe` process of its own on the standard fixture: `library create`, `import` (22 new identities), a
    second `import` that writes nothing (no identity, entry, disc group, history or review row), `db rebuild-index`, `db
    check`, `search nintendo` (13 games), `show`, `assign gb --profile fake-return`, `launch --game <Tiny Tennis> --wait`
    into the build's fake emulator (returned, exit 0, recorded and verified), `sessions`, `db check` again; every process
    with `RV_LAUNCH_BACKGROUND=1` and `RV_LAUNCH_ALLOWLIST` naming the fake alone; the fixture's files (sizes, times,
    SHA-256) the same afterwards.
- **The focus watch**: the whole gate runs under `python -I scripts/focus_watch.py --watch build` (the gate starts itself
  again under the watcher): a foreground change to a test process, a visible window of one, or a system error dialog
  during the run fails the gate. The watcher also counts a failing program of the user's (a WerFault window or a loader
  box it cannot attribute) against the run (BACKLOG "focus_watch.py in Gate A and the test script"): the row names each
  finding with the rule that matched, so that such a finding can be told from a test's; then run the gate again.
- **With `-RealLibrary`**, on the real ES-DE library (its folders from `-EsdeHome`, `-EsdeInstall`, `-RomRoot`,
  `-MediaRoot` or the `RV_REAL_*` variables; refused while ES-DE runs; nothing in the repository names them):
  1. the NFR-02 benchmark of I2 step 9 in Release (828 indexed queries on 10,000 generated games), before the
     snapshots since it reads no library;
  2. `scripts/real-library.ps1` (reused, not duplicated): read-only snapshots of the ES-DE home and install (hashed)
     and of the ROM and media roots (listed), then `scripts/gate-m1-real.ps1` (`-Measure`), then the hidden
     `[real-library]` tests of Release, then the snapshots again: **0 differences** required (NFR-10);
  3. `gate-m1-real.ps1`, into a fresh data directory under `build/gate-m1/real-measurements`: `retroverse-cli library
     create`, the ROM scan (`import --dry-run`, the first read of the library's files after the snapshot), the first
     import, a second one that must write nothing, `db rebuild-index`, `db check`, `media --coverage`, `search` and
     `show`, then NFR-01: `retroverse.exe` started three times on that data directory, hidden, without vsync and without
     `--fixed-dt`, the quick catalog opened on frame 1, the first page of the real catalog timed from the process's
     creation (the run report's `catalog.times.firstPageMs`), limit 5 s; with `-FullHashPass` also `identities hash`,
     the whole library's quick-hash pass (about 45 minutes cold, off by default); every process with the allow-list
     naming the build's fake emulator alone, so that no real emulator can start;
  4. the `[real-library]` tests (Release), among them NFR-01 again with a scroll through every game
     (`[real-library][app]`), NFR-02 on the real catalog (`[real-library][query]`: the benchmark's kinds of search with
     every order and a system filter, 564 queries on 90,246 games), the import, a second import, `db rebuild-index` and
     the ROM root unplugged and back (`[import]`), the quick-hash pass on a sample, gb (`[reconciliation]`), the ROM scan
     against the census, the media tree and coverage, ES-DE's configuration, gamelists and launch translation.

  Each figure is a row of the summary, with its limit where one exists; a row without a limit is `info`.

Every step logs into `build/gate-m1` (the build and test logs, `results.json`, `focus_watch.json`,
`real-library.log`, `real-library/` with the snapshots and `tests.log`, `real-measurements/` with `figures.json` and
each command's output and log).

## How to run it

```powershell
./scripts/gate.ps1 -Gate M1                         # fixtures only (about 20 minutes incremental)
./scripts/gate.ps1 -Gate M1 -RealLibrary -EsdeHome <ES-DE home> -EsdeInstall <ES-DE install> -RomRoot <ROM root> `
    -MediaRoot <media root> -RetroArchExe <RetroArch folder>\retroarch.exe   # also the real library, ES-DE closed
./scripts/gate.ps1 -Gate M1 -RealLibrary -FullHashPass   # also the whole library's quick-hash pass (about 45 min cold)
./scripts/gate.ps1 -Gate M1 -Clean                  # every target rebuilt first
```

Close ES-DE first, and run it on a quiet machine: the timings are the gate's figures. The gate exits 0 when no step
failed (a skipped step, no OpenGL 4.5 context or another GPU than the golden's, is reported, not failed), 1 otherwise,
2 when it cannot start (ES-DE running, Python missing, wrong options). `-RetroArchExe` (optional, or
`RV_REAL_RETROARCH_EXE`) lets the two launch-translation tests of `[real-library]` run (the readiness of the real
systems and the launches ES-DE logged, read from the git-ignored
`tests/real_library/local/esde_logged_launches.json`): they compute and never start anything;
without it they skip.

## The figures of this run

October 17, 2026, on the reference machine ([reference-hardware.md](reference-hardware.md): Intel Core i7-9700K, 8
cores, 64 GB, RTX 2080 SUPER with OpenGL 4.5.0 NVIDIA 617.42, Windows 11 Home 10.0.29671). The library: the ES-DE home
on the system SATA SSD, ES-DE's install and the media root on a second SATA SSD (with the repository), the ROM root on a
20 TB external USB hard drive (exFAT); ES-DE 3.4.1, 45 gamelists, 105,130 entries (90,246 games). Nothing else of the
project ran meanwhile (this step's builds were done before each run), so the figures count as taken on a quiet machine;
the user's own programs ran (the focus watcher saw 13 foreground changes, all to the user's windows, none to a test
process).

Both gate runs passed: `./scripts/gate.ps1 -Gate M1` (16 steps, 1,218 s) and `./scripts/gate.ps1 -Gate M1 -RealLibrary
... -RetroArchExe ...` (28 steps, 1,050 s, its builds incremental). The logs are in `build/gate-m1` (the earlier runs in
`build/gate-m1/earlier-runs`: the fixtures-only run, and a first `-RealLibrary` run without `-RetroArchExe`, whose two
launch-translation tests skipped and whose figures agree with these within a few percent).

| Step | Result | Limit | Details |
|---|---|---|---|
| Build `msvc-x64-debug`, `-relwithdebinfo`, `-release` | passed | 0 warnings | 0 warnings each |
| Unit tests Debug / Release | passed | | 1,266 passed each (289 s / 160 s) |
| GPU smoke tests Debug / Release | passed | | 28 / 26 passed (Release has neither the overlay nor the asset preview) |
| Golden G01 Debug / Release | passed | 0.5% | 0 of 2,073,600 pixels differ by more than 6/255 (largest difference 0/255) |
| M1 unit tests Debug / Release | passed | | 630 passed each (of the unit runs) |
| M1 GPU catalog and launch Debug / Release | passed | | 11 passed each (4 `gpu.catalog`, 7 `gpu.launch`) |
| M1 CLI as a process Debug / Release | passed | | `cli/process` passed (5 s / 4 s) |
| NFR-02 benchmark, 10,000 games | passed | <= 100 ms | p95 **3.44 ms** (p50 0.59, max 5.32) over 828 indexed queries |
| Real library: snapshots (NFR-10) | passed | 0 differences | **0 differences among 542,082 entries** (the ES-DE home and install hashed, the ROM and media roots listed) |
| Real library: `[real-library]` tests, Release | passed | | 12 test cases, 6,497,746 assertions |
| NFR-01, start to the first page | passed | < 5,000 ms | **1,585, 1,170 and 1,144 ms** from the process's creation (the catalog opened on the worker at 619 to 622 ms, its first list at 643 to 648 ms); the `[real-library][app]` test: 1,075 and 1,068 ms, then 6,099 frames scrolling the 90,246 games at p50 1.44 to 1.47 ms, p99 4.3 to 4.9 ms |
| NFR-02 on the real catalog | passed | <= 100 ms | p95 **42.05 ms** (p50 4.30, max 317.75: one pass of `arcade` in release-date order with the unavailable, 7,434 games, a single outlier; the next slowest `europe` 90 ms once, `nintendo`, 36,490 games, 65 to 68 ms) over 564 indexed queries on 90,246 games; a page of 30 rows p95 0.243 ms |
| ROM scan (`import --dry-run`) | info | | 2.0 s: listing 0.7 s (147,107 files, 191 systems), gamelists 0.5 s, sheets 0.03 s (2,520 read); **warm**: 51 h after the boot, the library read by earlier runs of the day (the snapshot before it lists the folders only) |
| First import | info | | 14.3 s (in the import 14.0 s: the library read 2.0 s, media 1.1 s, writes 10.1 s): 105,130 identities and entries, 2,513 disc groups |
| Second import | passed | nothing written | 5.6 s: 0 identities, 0 entries written or removed, 0 disc groups |
| `db rebuild-index` | info | | 14.7 s: 105,130 entries written again, 0 identities |
| Media coverage | info | | 2.7 s: 39,404 of 90,246 games with a medium (43.7%): covers 39.9%, screenshots 43.5%, miximages 43.5%, videos 36.1%, manuals 25.1%, fanart 21.6%; 13 issues, exit 0 |
| Quick-hash pass, sample (gb) | info | | 8,611 files, 980.9 MiB in 8.22 s, 1,047 files/s (**warm**; step 11b measured it cold: 268.8 s, 32 files/s); the whole library (103,937 files, 9,857.9 MiB) estimated at 83 to 99 s warm, about 45 minutes cold |
| Quick-hash pass, whole library | off | | not run (`-FullHashPass`) |
| Focus watch (whole gate) | passed | no finding | 13 foreground changes, 0 to test processes; 10 windows shown, 0 visible of test processes, 2 hidden or off-screen (the fake emulator's) |

The cold figures (the ROM scan's sheets after a reboot, estimated at 4 to 5 s by I2 step 7, and the whole library's
quick-hash pass, about 45 minutes) need a gate run that is the first large read after a boot: run `./scripts/gate.ps1
-Gate M1 -RealLibrary` (with `-FullHashPass` for the pass) right after restarting the machine, without flushing caches
by other means. NFR-01 and NFR-02 do not depend on it: the catalog is read from the database on the SSD.

## Requirements

M1's Done (specification §22): "Retroverse legge una collezione ES-DE esistente senza spostare ROM; selezione titolo e
avvio funzionano con processo esterno", over its scope: ES-DE discovery, gamelist parsing, the ROM and media folders,
the SQLite index and refresh; systems and the logical game, the metadata and media resolver, text search; a first
launcher for at least one configurable emulator.

| M1 scope | Where it is proven |
|---|---|
| ES-DE discovery | `esde/discovery: ...` (installed, portable, `--home`, the uninstall entry; fixtures `standard`, `portable`, `custom-dirs`), `cli/discover: ...`; the real installation in `[real-library]` (`esde/real: ...`) and the gate's `library create` |
| Gamelist parsing | `esde/gamelist: ...` (malformed, cut short, CRLF, BOM, the last entry counting), the real library's 45 gamelists (`esde/gamelist: the real library's gamelists`, `[real-library]`) |
| ROM and media folders | `esde/rom_scan: ...`, `esde/library_source: ...` (the real scan against `scripts/esde_census.py`, byte for byte), `esde/media: ...`; the gate's ROM scan and media coverage |
| SQLite index and refresh | `catalog_db/import: ...` (first import, refresh, rebuild, guard, unplugged folders), `cli/process` (a second import writes nothing), the gate's real imports (the second writing nothing) and `db rebuild-index`; M1's refresh is `retroverse-cli import` (the refresh on the app's worker, step 13r, moved to I3) |
| Systems, the logical game, metadata and media | `catalog/...` (ids, lookup keys, metadata), `catalog_db/import: the standard fixture imported`, `cli/import: ...`, `cli/media: ...`, the card in `gpu.catalog: ...` |
| Text search | `catalog_db/search: ...`, `cli/catalog: search ...`, `gpu.catalog: a game found by search and filter ...`; NFR-02 below |
| A launcher for a configurable emulator, launch and return | `cli/launch: ...`, `cli/assign: ...`, `cli/process`, `gpu.catalog: ... launches from the ES-DE catalog and returns where it was`, `gpu.launch: ...`; RetroArch by hand, isolated (I2 step 12) and in the manual session below |

The roadmap's requirement map (specification §18.1, §20, §23), each with where it is proven:

| Requirement | What it asks | Where it is proven |
|---|---|---|
| FR-002 | ES-DE's data imported: gamelists and the ROM root, installed or portable | `esde/discovery: a portable installation, its portable.txt read as ES-DE reads it`, `cli/discover: a portable installation, --output, problems and invalid options`, `catalog_db/import: the standard fixture imported`, `cli/process`; the real library: `[real-library]` and the gate's imports (105,130 entries, 90,246 games) |
| FR-003 | No ROM duplication: shared paths, nothing copied | `catalog_db/import: the standard fixture imported` (nothing of the fixture copied into the data directory), `catalog_db/import: a first import reads no ROM byte`, `cli/process` (the fixture unchanged); the real data directory holds the database alone (gate) |
| FR-004 | ES-DE's media used in place | `esde/media: ...` (every kind, ES-DE's extensions in its order, case rules), `esde/media coverage: ...`, `cli/media: ...`; `cli/media: the real library's media coverage against the oracle and ADR-0003` (`[real-library]`); the gate's media coverage |
| FR-011 | An external emulator started with the right ROM | `cli/launch: every outcome is recorded with its session row`, `cli/launch: hostile and long-path arguments reach the emulator byte-identical`, `esde/launch translation: ...`, `esde/real: the launches ES-DE logged, reproduced argument for argument` (`[real-library]`), `cli/process`, `gpu.catalog: a game found by search and filter launches ...`; RetroArch by hand (step 12) and the manual session (items 6-7) |
| FR-012 | The return restores the scene, the input and the session | `gpu.catalog: ... returns where it was` (selection, search, filter, readiness and camera equal before and after), `gpu.launch: a game launched from the quick catalog returns, and no frame runs while it plays`; the I1 return spike (30 of 30 RetroArch returns, ADR-0006); the manual session (item 7: fullscreen, the pad within about 1 s) |
| FR-013 | Global search over an index, no room loaded | `catalog_db/search: ...` (words in any order, prefixes, diacritics, FTS5 syntax inert), `cli/catalog: search ...`, `cli/process`, `gpu.catalog: ...`; NFR-02 |
| FR-014 | Everything navigable with the pad | `ui/nav_input: ...`, `ui/quick_catalog_model: ...`, `spike_ui.scenario` (153 checks with a virtual pad), `gpu.catalog: ...` (UI actions only); the manual session with a real pad. Deviation for M1: the search field takes typed text until the on-screen keyboard (I7, BACKLOG "Text entry with the pad"), so the manual session types its one search with the keyboard; the wizard and settings screens come in I7 |
| FR-018 | XML handled safely: nothing shared modified | `esde/xml...` (fragments, BOM, CRLF, truncation, the size cap), `cli/data-dir: nothing is written in the folders a library reads, not even the log file`; NFR-10 |
| FR-019 | Missing files: no crash, a reliable message or fallback | `catalog_db/import: ROMs added and removed between imports (SC-05)` (missing files kept as such), `esde/rom_scan: a missing ROM folder keeps its gamelist's entries as source unavailable`, `gpu.catalog: a game that is not ready shows its readiness, and Avvia shows why instead of launching`, `gpu.launch: failures leave the application interactive with their outcome`, `cli/launch: every outcome ...` (an executable that is not there, a game whose file is gone: blocked) |
| FR-020 | Configurable emulators: a test and recovery for ES-DE commands that do not resolve | `cli/assign: choices are checked against what exists, rank before emulators.json, and survive a restart ...`, `cli/launch: --wait follows each wait strategy to its end` (`--test`), `cli/profiles: the systems of the standard fixture, their commands and readiness`, `esde/real: the readiness of the real systems that have games` (`[real-library]`) |
| FR-026 | Game history: sessions and last launches | `catalog_db/sessions: ...`, `cli/launch: every outcome is recorded with its session row`, `cli/process` (`sessions`), `gpu.catalog: ...` (the session recorded and verified); the manual session (items 7 and 9) |
| FR-033 | Disconnected disks: nothing deleted for good by a scan | `catalog_db/import: folders unplugged or that cannot be listed keep their rows (SC-06)`, `catalog_db/import: folders that cannot be listed keep their rows as source unavailable`, `cli/import: the mass-disappearance guard exits 1 until the removals are accepted`; the real ROM root unplugged and back (`[real-library][import]`) |
| NFR-01 | Start with an indexed catalog within 5 s on the reference PC | the gate's NFR-01 row (three starts of `retroverse.exe`) and `app/catalog: the real library's catalog in retroverse.exe: NFR-01, page fetches and frame times` (`[real-library][app]`) |
| NFR-02 | An indexed query on 10,000 games within 100 ms | the gate's NFR-02 rows: `catalog_db/bench: NFR-02, ...` (10,000 generated games) and `catalog_db/query: NFR-02 on the real library's catalog: indexed queries and pages` (`[real-library][query]`, 90,246 games) |
| NFR-10 | The source metadata byte-identical in read-only mode | the gate's snapshot row (the ES-DE home and install hashed, the ROM and media roots listed, before and after everything the gate runs on the real library: 0 differences), `cli/process` (the fixture's files hashed before and after) |
| SC-02 | A cartridge selected, launched with the right ROM, the focus handed over, back with camera and selection | `gpu.catalog: a game found by search and filter launches from the ES-DE catalog and returns where it was`; the manual session (items 6-7). M1 selects in the quick catalog: the rooms and their shelves come with I3 |
| SC-05 | A ROM added: the catalog updates, nothing else moves | `catalog_db/import: ROMs added and removed between imports (SC-05)` (by `retroverse-cli import` in M1; the file watcher is I7, NFR-05) |
| SC-06 | The external drive unplugged: "source unavailable", nothing deleted, the same ids back | `catalog_db/import: folders unplugged or that cannot be listed keep their rows (SC-06)`; the real ROM root unplugged and back (`[real-library][import]`) |
| SC-07 | Two frontends: a game renamed in ES-DE is reconciled, ambiguity shown | `catalog_db/reconciliation: SC-07, a file renamed, moved to another folder, moved with its gamelist entry ...`, `cli/identities: the quick-hash pass, then SC-07 on disk ...`, `cli/reconciled sessions: SC-07, moved identities keep their GameIds ...` |
| SC-10 | A launcher that is not configured: no arbitrary command, a choice of emulator, the profile recorded when its test succeeds | `gpu.catalog: a game that is not ready shows its readiness, and Avvia shows why instead of launching` (`requires-configuration`), `esde/launch translation: ...` (the shell commands refused), `cli/assign: ...`, `cli/launch: ...` (`--test` records a verification) |
| SC-11 | An unknown system: kept and launchable with a valid profile | `catalog_db/import: the user's custom systems are imported like any other (SC-11)`, `esde/systems: custom systems replace bundled ones ...`, `cli/launch: every outcome ...` (a profile serving any system) |
| SC-12 | A large library: search without materializing the world, no flood of work | `ui/quick_catalog_model: ...` (a window over 100,000 games), `spike_ui.scenario` (100,000 games), `catalog_db/bench: a UI page of 30 rows fetched by ids within 5 ms on 100,000 games ...`, the real catalog scrolled through its 90,246 games (`[real-library][app]`), the page fetches of the NFR-02 row |

Deferred (roadmap): the 100/1k/10k/30k import fixtures of specification §18.3 beyond the search benchmark (I8);
NFR-05's file watcher (I7); the refresh on the app's worker (step 13r, I3).

## Manual session

The one part of the gate a script cannot do: a real gamepad, a real emulator through an isolated profile, and the
user's eyes. With ES-DE closed, on the reference machine, from the Release build (`./scripts/build.ps1 -Preset
msvc-x64-release`). Retroverse only reads the library; RetroArch runs with an appended configuration that keeps
everything it would write inside the data directory's scratch folder (I2 step 12's isolation; ADR-0008 §8).

**Preparation** (once, in PowerShell from the repository root):

1. Choose a user data directory outside the library's folders, e.g. `D:\RetroverseM1` (the CLI refuses one inside the
   ES-DE home, the install, the ROM or the media root).
2. Run `./scripts/m1-manual-setup.ps1 -DataDir <data dir> -EsdeHome <ES-DE home> -EsdeInstall <ES-DE install>
   -RetroArchExe <RetroArch folder>\retroarch.exe` (add `-RomRoot`/`-MediaRoot` only if ES-DE's settings do not give
   them; `-System gb -Core gambatte_libretro.dll` are the defaults). It runs, with the Release `retroverse-cli`:
   `library create`, `import`, writes the isolated RetroArch profile (`<data dir>\retroarch-scratch\retroverse-isolated.cfg`
   and `<data dir>\config\emulators.json` with the profile `retroarch-isolated`: `-L <core> --appendconfig=<cfg>
   --sram-mode=noload-nosave --log-file=<scratch>\logs\retroarch.log <rom>`), `assign gb --profile retroarch-isolated`
   and `profiles --system gb` (the profile ready), and prints the two commands below. By hand instead: the same
   `retroverse-cli --data-dir <data dir>` commands, the configuration as the script writes it.
3. In the same PowerShell: `$env:RV_LAUNCH_ALLOWLIST = '<RetroArch folder>\retroarch.exe'` (the automation guard: only
   RetroArch may start, and only with its `--appendconfig` file), then
   `build/msvc-x64-release/bin/retroverse.exe --data-dir <data dir> --fullscreen`.

**Checklist** (with the pad only from here, except where an item says otherwise; sign and date each line):

| # | Item | Signed | Date |
|---|---|---|---|
| 1 | Within 5 s of the start, Y opens the quick catalog on its first page of games (no "Apertura del catalogo..." left showing); the count reads about 90,000 games | | |
| 2 | The D-pad and the left stick move through the list; held, it scrolls and the card follows once the selection rests; the triggers page; the stick clicks jump to the first and the last game | | |
| 3 | Search, with the keyboard (the one step that is not pad-only in M1: the on-screen keyboard comes in I7): from the first game, up twice to the search field, type a word of a known title; the list narrows to the matching games and the count follows | | |
| 4 | The shoulders change the system filter: the chip row shows the active system, the list and the count follow; a filter and the search together | | |
| 5 | A game whose emulator is missing or needs configuration (e.g. a ps3 or ps2 game) shows it on its card; A on it shows why in the game-session panel and launches nothing; B dismisses the panel | | |
| 6 | A on a ready gb game: "Gioco in corso", RetroArch in front in fullscreen, playable with the same pad; Retroverse renders nothing meanwhile | | |
| 7 | Quit RetroArch from its menu with the pad: Retroverse is back in front, in fullscreen, with the same game selected, the same filter and search and the walker where it was, and the pad works within about 1 s | | |
| 8 | Launch again; while the game runs, Alt+Tab (or the Windows key) back to Retroverse: after about 1.5 s "Gioco in corso" asks; "Torna al gioco" goes back to the game | | |
| 9 | Quit Retroverse while the game runs (close its window from the taskbar, the one step that needs the mouse): the game keeps running; quit RetroArch afterwards | | |
| 10 | `build/msvc-x64-release/bin/retroverse-cli.exe --data-dir <data dir> sessions` lists the session of item 7 as returned (verified) and the one of item 9 as left running (quit) | | |
| 11 | Start Retroverse again with the same data directory and open the catalog: B walks back out of it to the room, and the walker moves with the stick | | |
| 12 | RetroArch wrote only into `<data dir>\retroarch-scratch` (its log, core options); ES-DE's files untouched (optional: `python -I scripts/snapshot_tree.py snapshot` of the ES-DE home and of RetroArch's folder before preparation and after, `compare`: 0 differences) | | |

Signed off by: ______________________  Date: __________
</content>
