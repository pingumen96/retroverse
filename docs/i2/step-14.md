# I2 step 14: Gate M1

Roadmap row: I2 step 14 (after the wave-4 merge, the last step of I2). Gate M1 closes milestone M1 (specification §22:
Retroverse reads an existing ES-DE collection without moving anything, and a title selected in the quick catalog
launches in its emulator and comes back). This step built the gate's script, ran it on the fixtures and on the real
library, wrote the gate's page (`docs/testing/gate-m1.md`, with the manual checklist the user signs) and folded step
13's notes into the shared documentation. M1 is not marked done: the gate passed, the manual checklist is pending.

## What was built

| File | What |
|---|---|
| `scripts/gate.ps1` | `-Gate M1`: Gate A's steps, rows for the M1 unit tests and the `gpu.catalog`/`gpu.launch` cases of Debug and Release (counted from those runs' JUnit reports), the unit test `cli/process` per preset; `-RealLibrary` (with `-EsdeHome`, `-EsdeInstall`, `-RomRoot`, `-MediaRoot` or the `RV_REAL_*` variables, refused while ES-DE runs) and `-FullHashPass`; the whole gate under `scripts/focus_watch.py`; a `Limit` column for the figures that have one, `info` rows for those that do not |
| `scripts/gate-m1-real.ps1` | the measurements on the real library, run by `real-library.ps1 -Measure` between its snapshots: `library create`, the ROM scan (`import --dry-run`), two imports, `db rebuild-index`, `db check`, `media --coverage`, `search`, `show`, NFR-01 with `retroverse.exe` (three starts), `identities hash` with `-FullHashPass`; `figures.json` |
| `scripts/real-library.ps1` | `-OutDir` (the outputs elsewhere than `build/real-library`), `-Measure <script>` with `-MeasureArguments` (run between the first snapshot and the tests, its exit code counted) |
| `scripts/m1-manual-setup.ps1` | prepares the manual session: a data directory with the real library recorded and imported, the isolated RetroArch profile and `assign` (nothing started) |
| `tests/unit/cli/test_cli_m1_flow.cpp` | `cli/process: the M1 flow on the standard fixture, every command a process of its own` |
| `tests/unit/catalog_db/test_catalog_query_real.cpp` | `[.real-library][query]`: NFR-02 on the real catalog |
| `docs/testing/gate-m1.md` | the gate's page: what it checks, how to run it, the figures, the requirement map, the manual checklist |
| `CLAUDE.md`, `docs/architecture.md`, `docs/roadmap.md`, ADR-0005, ADR-0008, `BACKLOG.md` | step 13 folded in (below), Gate M1 |

## How to use it

```powershell
./scripts/gate.ps1 -Gate M1                         # the fixtures: about 20 minutes with incremental builds
./scripts/gate.ps1 -Gate M1 -RealLibrary -EsdeHome <ES-DE home> -EsdeInstall <ES-DE install> -RomRoot <ROM root> `
    -MediaRoot <media root> -RetroArchExe <RetroArch folder>\retroarch.exe     # ES-DE closed; RetroArch never started
./scripts/gate.ps1 -Gate M1 -RealLibrary -FullHashPass                         # also the whole quick-hash pass
./scripts/real-library.ps1 -Filter '[real-library][query]'                     # NFR-02 on the real catalog alone
./scripts/m1-manual-setup.ps1 -DataDir <data dir> -EsdeHome <ES-DE home> -EsdeInstall <ES-DE install> `
    -RetroArchExe <RetroArch folder>\retroarch.exe                             # the manual session's preparation
```

The folders can also come from the `RV_REAL_*` variables. The gate's page, `docs/testing/gate-m1.md`, says what each row
checks; the logs are in `build/gate-m1` (`results.json` holds the rows, `focus_watch.json` the watcher's report,
`real-measurements/figures.json` the figures of the real library).

## The figures

Both runs passed on 2026-10-17 (the reference machine, nothing else of the project running; the user's own programs
ran: 13 foreground changes, none to a test process). `-Gate M1`: 16 steps in 1,218 s. `-Gate M1 -RealLibrary
-RetroArchExe`: 28 steps in 1,050 s, among them:

| Figure | Value | Limit |
|---|---|---|
| NFR-01, start to the first page of the real catalog (3 starts) | 1,585, 1,170, 1,144 ms | < 5,000 ms |
| NFR-02, 10,000 generated games (828 queries) | p95 3.44 ms | <= 100 ms |
| NFR-02, the real catalog (564 queries, 90,246 games) | p95 42.05 ms, max 317.75 ms (one outlier) | <= 100 ms |
| NFR-10, snapshots of the library before and after | 0 differences among 542,082 entries | 0 |
| First import / second import / `db rebuild-index` | 14.3 s / 5.6 s (nothing written) / 14.7 s | |
| ROM scan / media coverage | 2.0 s / 2.7 s (43.7% of the games with a medium) | |
| Quick-hash sample, gb | 8,611 files in 8.22 s (warm) | |

Everything on the real library was warm (51 hours after the boot, the library read by earlier runs of the day): the
cold ROM scan and the cold whole-library quick-hash pass were not measured, since caches are never flushed by other
means; they need a gate run right after a reboot (`-FullHashPass` for the pass). Measured while another preset built
(a trial before the gate's runs), NFR-01 took 5.9 to 7.3 s and the real NFR-02 p95 101 ms: both figures depend on a
quiet machine (NFR-01's time is mostly the scene's loading, the procedural surfaces generated on every core).

## Decisions taken within the step

1. **The M1 rows count the presets' own runs.** The catalog's unit tests and the `gpu.catalog`/`gpu.launch` cases are
   already in the unit and gpu presets: the gate counts them from those runs' JUnit reports by name (a row each per
   preset) instead of running them twice.
2. **`retroverse-cli` as processes is a unit test** (`cli/process`, in every unit run), which the gate also runs alone
   per preset: the fixture materializer is C++, and the flow's checks read the commands' JSON.
3. **`real-library.ps1` gained `-Measure`** rather than the gate copying its preconditions and snapshots: the
   measurement script runs between the first snapshot and the tests, so that its ROM scan is the first read of the
   library after the snapshot, and it counts like the tests. `-OutDir` keeps the gate's outputs in `build/gate-m1`.
4. **NFR-01 is measured by the gate itself** on the data directory its own `retroverse-cli` made (three starts, 3,000
   frames each so that a slow opening still shows its first page), besides the `[real-library][app]` test.
5. **NFR-02 on the real catalog** is a new hidden test with the 100 ms limit applied to 90,246 games, nine times the
   specification's 10,000; the benchmark on 10,000 generated games stays the requirement's own figure.
6. **The gate watches itself**: `-Gate M1` starts itself again under `focus_watch.py --watch build` and prints the
   inner run's rows (`results.json`) with the watcher's verdict; a finding fails the gate, its row naming the rule that
   matched (a failing program of the user's counts too: BACKLOG). Gate A is unchanged.
7. **Rows without a limit are `info`** (neither pass nor fail); the whole quick-hash pass is `off` unless asked.
8. **`-RetroArchExe`** reaches the two launch-translation tests of `[real-library]` (the readiness of the real systems,
   the launches ES-DE logged), which compute and start nothing; without it they skip. The second also reads the
   git-ignored `tests/real_library/local/esde_logged_launches.json`, which a fresh worktree has to get from the main
   checkout.
9. **Orchestrator decisions recorded**: step 13r (the refresh on a worker) moves to I3 (roadmap row 13r, BACKLOG
   "Plan"; M1 refreshes with `retroverse-cli import`); M1's manual session searches with the keyboard, every other item
   pad-only (BACKLOG "Text entry with the pad", ADR-0005's amendment; the on-screen keyboard is I7).
10. **The manual session's RetroArch profile** is step 12's isolation without the frame limit and with sound (a person
    plays it), written by `scripts/m1-manual-setup.ps1`, which starts nothing. Tried on the real library between the snapshots of
    `real-library.ps1 -Measure` (0 differences): the profile ready for gb, and `launch --dry-run --game` of a gb game
    with the allow-list naming RetroArch shows the isolated command line and the guard's "allowed"; RetroArch was not
    started.

## Step 13 folded into the shared documentation

- `CLAUDE.md`: the quick catalog over the ES-DE catalog (the worker, the notices, the lockstep of `--fixed-dt` runs),
  input scripts' `"text"`, the run report's catalog and launch records, the GPU catalog cases, `rv_app_catalog` in the
  layout, the `[real-library][app]` and `[query]` filters, `-OutDir` and `-Measure`, Gate M1.
- `docs/architecture.md`: the app row, `rv_app_catalog` among the GPU-free sub-targets and in the module diagram, the
  catalog worker's threading with its diagram, the GPU catalog cases, Gate M1.
- `docs/roadmap.md`: step 13 done with NFR-01's figures, 13r moved to I3, step 14 "gate passed, manual checklist
  pending", the requirement map linked to the gate's page.
- ADR-0008 §5 (amended: the lockstep of `--fixed-dt` runs) and ADR-0005 (amended: what the card shows, never cut; the
  keyboard search of M1's manual session).
- `BACKLOG.md`: the app's log before the data-directory guard, one launch library for the CLI and the app, a refresh
  when another process imports, readiness in bulk, the on-screen keyboard's note, 13r and the whole quick-hash pass in
  "Plan", `focus_watch.py` in Gate A and the test script.

## What the user must do

1. **The manual session** (`docs/testing/gate-m1.md`, "Manual session"): prepare it with `scripts/m1-manual-setup.ps1`,
   play a gb game through the isolated RetroArch profile with a real pad, and sign and date each of the 12 items. M1 is
   done once they are signed; then mark it done in the roadmap (row 14 and the I2 heading) and in the gate's page.
2. **Optional, the cold figures**: right after a reboot, `./scripts/gate.ps1 -Gate M1 -RealLibrary -FullHashPass` (about
   an hour), for the cold ROM scan and the whole library's quick-hash pass.
3. **Schema v1**: it "stays open until the M1 gate" (CLAUDE.md); once M1 is signed it closes, and later schema changes
   are migrations to v2 (the sentence in CLAUDE.md and `docs/architecture.md` changes with the sign-off).