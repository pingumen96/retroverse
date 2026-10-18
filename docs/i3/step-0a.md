# I3 step 0a: `rv_world` and the I3 skeletons

Step 0a of I3 (docs/roadmap.md, "I3: M2 SNES vertical slice") settles the files that the later steps would otherwise
edit together: the new library `rv_world` with its layer rules, every source file the I3 steps plan (in `rv_world` and
in the other modules) as a stub that names the step filling it, the CLI's `worldplan` and `where-is` commands, the
app's new options, the run report's new sections, glad's `GL_EXT_texture_sRGB`, the data files under `assets/world`
and the fixture folders, all of them declared and empty. Nothing of I3's behaviour exists yet: the stubs exit 2, the
options are refused, the sections are empty, and golden G01 is untouched.

## What was built

### `rv_world` (`src/world`)

A static library, GPU-free (ADR-0010 §1), on `rv_core`, `rv_geometry`, `rv_assets`, `rv_scene` and `rv_catalog`. Its
forbidden links, besides the GPU libraries every GPU-free target is kept from (SDL3, glad, Dear ImGui), are in
`src/world/forbidden_links.cmake` (`RV_WORLD_FORBIDDEN_LINKS`): RmlUi (`RV_RMLUI_LIBRARIES`, new in
`cmake/LayerRules.cmake`), SQLite, pugixml, `rv_media`, `rv_ffmpeg`, `rv_esde`, `rv_catalog_db`, `rv_platform_win`,
`rv_platform`. The file is shared by `src/world/CMakeLists.txt` and the layering test, so the test checks the real list.

| Public header (`rv/world/`) | Step | What it will hold |
|---|---|---|
| `ids.hpp` | 1a | RoomId, FixtureId, SlotId, a dressed level's socket (ADR-0010 §7) |
| `world_plan.hpp` | 1a | WorldPlan: rooms, fixtures, slots, placements, dressed levels, unplaced with reasons, versions, digests |
| `plan_json.hpp` | 1a | the canonical writer and reader, the plan digest |
| `world_config.hpp` | 1a | WorldConfig, presets and `<user data>/config/world.json` |
| `room_template.hpp` | 1a | RoomTemplate and its reader |
| `platform_manifest.hpp` | 1a | the platform visual manifest and its reader |
| `media_family.hpp` | 1a | the media family registry and its reader |
| `room_input.hpp` | 1a | RoomInput (header only) |
| `room_selection.hpp` | 1a, 2 | RoomSelection (1a); `selectRoom` (2) |
| `title_key.hpp` | 2 | title keys, region ranks, status tags, revisions |
| `generation.hpp` | 1a, 3 | GenerationResult and the reports (1a); `generate` (3) |
| `capacity.hpp` | 3 | capacity from the prefabs' slots, the display rule's module count, growth |
| `seeds.hpp` | 3 | roomSeed, fixtureSeed, propSeed, placementSeed |
| `validation.hpp` | 4 | `validatePlan`, hard and soft rules |
| `nav_grid.hpp` | 4 | the 0.1 m navigation grid |
| `pick_index.hpp` | 7 | PickIndex |
| `slot_navigation.hpp` | 7 | the D-pad steps of Focus |
| `incremental.hpp` | 9 | the incremental update and its change report |
| `overrides.hpp` | 9 | `overrides.json` (pins) |
| `world_store.hpp` | 9 | the files under `worlds/<library id>/`, backups, the world lock |
| `where_is.hpp` | 9 | the plan's index |
| `scene_builder.hpp` | 10a | `toSceneDescription` |
| `collection_layout.hpp` | 10a | `collectionLayout` |

Private sources with their headers in `src/world/src`: `allocation` (3), `fixture_layout` (3), `layout_score` (4).
Every header but `room_input.hpp` has its `.cpp` in the source list, which only includes the header today.

### The other modules

| Module (target) | Files (header and source unless noted) | Step |
|---|---|---|
| `rv_core` | `seed`, `random`, `canonical_json`, `worker_pool`, `backups` | 0b |
| `rv_core` | `cpu_timers` | 0d |
| `rv_render_uniforms` | `gpu_memory` | 0d |
| `rv_app_cli` | `frame_timing` | 0d |
| `rv_catalog` | `room_entry.hpp` (header only) | 1a |
| `rv_geometry` | `prefab_display_case.cpp`, `prefab_wall_shelf.cpp`, `prefab_shelf_dressing.cpp` (on `prefabs.hpp`) | 1b |
| `rv_catalog_db` | `room_query.cpp` (on `catalog_query.hpp`) | 2 |
| `rv_assets` | `cover_fit`, `mip_chain`, `bc1`, `text_raster`, `collection_labels`, `rvtex`, `derived_texture_cache` | 5 |
| `rv_render_uniforms` | `texture_array_layout` | 6 |
| `rv_render` | `texture_array`, `collection_arrays` | 6 |
| `rv_scene` | `pose_animator` | 7 |
| `rv_app_cli` | `world_interaction` | 7 |
| `rv_scene` | `instance_clusters` | 8 |
| `rv_render` | `static_instance_group`, `outline_pass` | 8 |
| `rv_app_cli` | `screen_policy` | 11 |
| `rv_app` | `screen_service` (private header) | 11 |
| `rv_app_catalog` | `room_snapshot` | 10b |
| `rv_app` | `world_session`, `collection_instance` (private headers) | 10b |
| `rv_app` | `world_start` | 10c |
| `rv_app_cli` | `cover_requests` | 12 |
| `rv_app` | `cover_loader`, `hero_textures` | 12 |
| `rv_app` | `world_hover`; `rv_ui`: `hover_label_screen` | 13a |
| `rv_app` | `world_inspection` | 13b |
| `rv_app_catalog` | `library_refresh` | 13r |
| `rv_ui_model` | `game_card_model`; `rv_ui`: `game_card_screen`; `rv_app`: `world_card` | 14a |
| `rv_ui_model` | `media_viewer_model`; `rv_ui`: `media_viewer_screen`, `game_info_screen` | 14b |
| `rv_app` | `world_launch` | 15a |
| `rv_app` | `world_live_update` | 15b |
| `rv_cli_core` | `cmd_worldplan_generate.cpp`, `cmd_worldplan_show.cpp` (3), `cmd_worldplan_check.cpp` (4), `cmd_worldplan_update.cpp`, `cmd_where_is.cpp` (9) | 3, 4, 9 |

`rv_app`'s new headers are private (`src/app/src/<name>.hpp`, as `product_ui.hpp`); those of `rv_app_cli` and
`rv_app_catalog` are public, so that `rv_tests` reaches them. Tests (`tests/unit/<module>`, `tests/smoke`,
`tests/support`) are globbed and need no list. Shaders and RML documents are files under `assets/`, in no list.

### Links

- `rv_app_cli` links `rv_world` (PUBLIC); `rv_app` links `rv_media` (the hero CRT's decoder, step 11) and `rv_world`;
  `rv_cli_core` links `rv_world` (PRIVATE); `rv_tests` links `rv_world`.
- `rv_test_support` links `rv_assets` and `rv_media`, for step 1c's payload generators (covers through the image
  encoder, clips through `media::test_video`). Static libraries: an executable that uses neither pulls nothing in.

### `retroverse-cli`

- `worldplan` (`cmd_worldplan.cpp`) dispatches `generate`, `update`, `show` and `check`, one source each
  (`cmd_worldplan_<name>.cpp`), so that steps 3, 4 and 9 never edit one file. Without a subcommand it writes its usage
  to stderr (exit 2), with `--help` to stdout (exit 0); an unknown subcommand or option is named (exit 2). Each
  subcommand answers, whatever its arguments, `retroverse-cli worldplan <name>: not implemented yet (I3 step <n>)`,
  exit 2: generate and show 3, check 4, update 9.
- `where-is` (`cmd_where_is.cpp`): `retroverse-cli where-is: not implemented yet (I3 step 9)`, exit 2.
- The command table's `step` became a string with its iteration (`"I2 step 9"`, `"I3 step 3"`), and
  `notImplemented(context, subcommand, step)` answers for one subcommand.

### `retroverse.exe`'s options

Parsed and validated, listed by `--help`, then refused by `pendingOptionError` (ErrorCode::Unsupported, "not
implemented yet (I3 step <n>)", the option as context): `parseCommandLine` fails, so the run exits 2 like any invalid
command line and writes no run report.

| Option | Validation | Step |
|---|---|---|
| `--refresh-library` | a flag | 13r |
| `--video-freeze-pts <seconds>` | frame-limited runs only; a finite number in [0, 86400] | 11 |
| `--no-texture-compression` | a flag | 6 |

### The run report

`RunReport` gained `world` (WorldRecord, step 10b), `residency` (ResidencyRecord, 12), `video` (VideoRecord, 11),
`phases` (a vector of PhaseRecord, which carries only its `name` until 0d) and `gpuMemory` (GpuMemoryRecord, 0d). Every
report writes them, as `{}` and `[]` today; `parseRunReport` requires them like every non-optional field (a missing one,
or one of another type, is refused with its JSON pointer), so every smoke case that reads a report proves they are
there, and `requireCleanRun` (tests/smoke/smoke_support.cpp) checks that they are empty, one line per section naming
the step that fills it.

### glad

Regenerated by its README's procedure (glad2 2.0.8 in a scratch virtual environment) with `GL_EXT_texture_sRGB` added
to the extension list. glad2 emits that extension's enums under the core profile (`GL_COMPRESSED_SRGB_S3TC_DXT1_EXT`
0x8C4C and its siblings, `GLAD_GL_EXT_texture_sRGB`), so `rv_render` declares nothing itself. Generating with the old
command first reproduced the vendored files exactly but for line endings (the generator writes CRLF; the repository
keeps LF, `.gitattributes`), so the diff of `third_party/glad` is the extension alone. The README and
`docs/third-party.md` name it.

`gl::GLCapabilities` gained `extTextureCompressionS3tc`, `extTextureSrgb` and `s3tcSrgb()`; `GLDevice::create` logs
them on its extensions line (`GL extensions: KHR_debug yes, ARB_clip_control yes, EXT_texture_compression_s3tc yes,
EXT_texture_sRGB yes` on the reference GPU) and warns when BC1 sRGB is not available. The golden case checks that line
on the reference machine.

### Data files and fixtures

- `assets/world/templates/console_room_9x10.json`, `assets/world/platforms/snes.json`,
  `assets/world/presets/retro_museum_cozy.json`, `assets/world/media_families.json`: `{}` with a comment naming what
  step 1a writes there. They reach the Release asset mirror like every asset; nothing reads them yet.
- `tests/fixtures/esde/snes-room`, `snes-single`, `snes-stress`: a README each, naming what step 1c builds (counts, the
  invented system name, generated covers and clips, nothing commercial). No `files.json` yet: nothing materializes them.
- `docs/i3/` (this file).

## Tests

- `tests/unit/world/test_world_module.cpp`: `rv_world` links into `rv_tests` and every new public header of I3's
  GPU-free code (the world's and those of `rv_core`, `rv_scene`, `rv_assets`, `rv_catalog`, `rv_render_uniforms`,
  `rv_app_cli`, `rv_app_catalog`, `rv_ui_model`) compiles in one translation unit.
- `tests/unit/cli/test_cli_commands.cpp`: the table's 16 rows with their steps; through the dispatcher, each worldplan
  subcommand (with no arguments, with options, with `--help`) and `where-is` exit 2 with their step on stderr and
  nothing on stdout or under `worlds/`; worldplan's usage, an unknown subcommand and an unknown option.
  `unit.cmake/cli_process` runs every command of `--help`'s table as a process (`worldplan` and `where-is` included).
- `tests/unit/app/test_command_line.cpp`: each option refused with its step and context, its value validated first
  (interactive, out of range, not a number, a flag given a value), `pendingOptionError` itself, `--help` listing them.
- `tests/unit/app/test_run_report.cpp`: the sections in every report (the minimal one too), empty; phases read back; a
  report without a section, or with one of another type, refused.
- `tests/cmake/test_layer_rules.cmake` (`unit.cmake/layer_rules`): a project without languages, written under the
  build tree, with stand-ins under the real target names and `rv_world` marked as the real one (the real
  `LayerRules.cmake` and `forbidden_links.cmake`), reaching one library through a first-party target in between. Without
  a forbidden link, or with an allowed one, it configures; with SDL3, glad (through its alias), Dear ImGui, RmlUi,
  SQLite, pugixml, `rv_media`, `rv_esde` (through its alias), `rv_catalog_db`, `rv_platform_win` or `rv_platform` the
  configure fails with "Layer rule violated" naming `rv_world`. About 1 s.
- `tests/smoke`: `requireCleanRun` checks the empty sections in every clean run; `golden/G01` checks the extensions line
  of the log on the reference machine.

Results (reference machine, RTX 2080 SUPER): see "For the merge" below for the figures of the final run.

## Decisions taken within the step

1. **Planned files for the main steps too.** The roadmap has steps 0a to 1b settle the shared files between them; 0a
   pre-lists the files of 0b, 0d, 1a and 1b as well, where the ADRs name them, so that their names are fixed early.
   Those steps run alone on main and may still rename a stub or add a file; the wave steps should not need to.
2. **No `rv_esde` stub.** `esde::mediaRelativePath` exists since I2 step 8; step 2 derives the room's media paths with
   it (near `media_resolver.cpp`, or in the catalog-side job of step 10b's `room_snapshot`).
3. **`Command::step` is a string** with its iteration: I3's steps carry letters (13r, 1a) and both iterations share the
   table. `worldplan`'s row names step 3, its first subcommands' step; each subcommand names its own.
4. **Options refused at parse time**, after their values are validated, so a script that passes one fails at once
   (exit 2) instead of running without it. `pendingOptionError` keeps one block per option with a blank line between
   them: steps 6 and 13r (both in wave 1) and 11 (wave 2) each delete their own block without a merge conflict.
5. **The run report's sections are required keys**, written even when empty, so that a report from a build that lacks
   a section is refused rather than read as "nothing happened". The format's `schemaVersion` stays 1 (fields are only
   added within it); no report of an earlier build is read by anything that must keep working.
6. **The world's forbidden list is a CMake file of its own**, shared by the target and its test, and RmlUi's target
   names joined `LayerRules.cmake` (`RV_RMLUI_LIBRARIES`) beside SQLite's and pugixml's. `rv_ffmpeg` is listed with
   `rv_media`, although only `rv_media` links it.
7. **`rv_test_support` links `rv_assets` and `rv_media` now**, for step 1c (wave 1), so that 1c adds globbed sources only.
8. **A warning when BC1 sRGB is missing.** The capability is logged at start on every run; without both extensions the
   log warns that the collection's textures will use RGBA8 (ADR-0011 §4's fallback, built in step 6).
9. **CLAUDE.md is not edited here** (see "For the merge").

## Notes for later steps

- A stub header holds one comment naming its step; replace it whole. A stub source only includes its header; keep the
  file in its list. If a planned file turns out unnecessary, leave it as a stub and say so in the step's notes rather
  than deleting it inside a wave (the list belongs to every worktree of the wave).
- Steps 3, 4 and 9 each fill their `cmd_worldplan_<name>.cpp`; `cmd_worldplan.cpp` only dispatches and needs no change.
  The help line of `worldplan --help` lists the subcommands already. A shared helper for the subcommands (reading a plan
  file, the snapshot from the database) would be a new file: add it on main, or in one step and merge that step first.
- Steps 6, 11 and 13r remove their block from `pendingOptionError` (src/app/src/command_line.cpp) and the matching
  "Not implemented yet" sentence of the option's `--help` text, and change the refusal test into their own.
- Steps 0d, 10b, 11 and 12 fill their run report record, its JSON in `formatRunReport` / `parseRunReport`, and change
  their line in `requireCleanRun`.
- Step 6 reads `GLCapabilities::s3tcSrgb()` for its fallback.
- Step 1c gives the three fixture folders their `files.json` and rewrites the READMEs with the counts its tests assert.
- Step 1a replaces the four `assets/world` placeholders with the real files and their readers.

## For the merge

- **CLAUDE.md** (not edited by the step):
  - "retroverse-cli": "Every command of the table is implemented" becomes: every command of I2 is implemented;
    `worldplan generate | update | show | check` and `where-is <gameId>` (I3, ADR-0010) answer "not implemented yet
    (I3 step <n>)" with exit 2 until their steps (generate and show 3, check 4, update and where-is 9).
  - "Running the app": `--refresh-library` (13r), `--video-freeze-pts <seconds>` (frame-limited runs; 11) and
    `--no-texture-compression` (6) are parsed and validated but refused (exit 2) until their steps; the run report
    holds the I3 sections `world`, `residency`, `video`, `phases` and `gpuMemory`, empty until their steps; the log's
    extensions line names `EXT_texture_compression_s3tc` and `EXT_texture_sRGB`.
  - "Layout and layering": `world` in the dependency order (on `core`, `geometry`, `assets`, `scene`, `catalog`; never
    RmlUi, SQLite, pugixml, `media`, `esde`, `catalog_db`, `platform_win` or `platform`: `src/world/forbidden_links.cmake`,
    proven by `tests/cmake/test_layer_rules.cmake`); "`rv_app` ... does not use `rv_media` yet" becomes: `rv_app` links
    `rv_media` (step 11) and `rv_world` (also linked by `rv_app_cli` and `rv_cli_core`); the I3 stubs pre-listed by
    step 0a (this file's tables); `tests/cmake` lists `unit.cmake/layer_rules`.
- **docs/architecture.md**: done in the step (the `world` row, the GPU-free list and the forbidden links, the diagram,
  `rv_app`'s links, the CLI rows).
- **docs/roadmap.md**: step 0a done.
- **docs/third-party.md** and **third_party/glad/README.md**: done in the step.
