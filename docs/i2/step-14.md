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
