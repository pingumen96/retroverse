# Layout and layering

Reference moved out of `CLAUDE.md` (2026-10-20); `CLAUDE.md` keeps the essentials and points here.

- `src/<module>/include/rv/<module>/*.hpp` public headers, `src/<module>/src/*.cpp`, `src/<module>/CMakeLists.txt`
  with an explicit source list. Include as `#include <rv/<module>/<file>.hpp>`.
- Dependency order: `core` -> `geometry` -> `input` -> `assets` -> `scene`, `media` (FFmpeg only inside its sources),
  `launcher` (GPU-free; `rv_forbid_link` fails the configure if they reach SDL3, glad or ImGui) -> `catalog` (I2,
  ADR-0008: the catalog's domain and ports, on `launcher`; never SQLite, pugixml or the Windows-only targets) -> `esde`
  (the ES-DE adapter, pugixml private behind `src/esde/src/xml.hpp`), `catalog_db` (the SQLite store, SQLite private),
  `world` (I3, ADR-0010: the world generator, GPU-free, on `core`, `geometry`, `assets`, `scene` and `catalog`; never
  RmlUi, SQLite, pugixml, `media`, `esde`, `catalog_db`, `platform_win` or `platform`:
  `src/world/forbidden_links.cmake`, proven by `unit.cmake/layer_rules`), `platform_win` (Windows without SDL, namespace
  `rv::platform_win`: the system probe `Win32SystemProbe`, the process runner `Win32ProcessRunner`, `win32_util.hpp`; on
  `core` and `launcher`) -> `platform` (SDL3; the window hand-over; links `platform_win`) -> `render` (OpenGL through
  glad; never includes SDL) -> `devui` (Dear ImGui, developer builds; its GPU-free part `rv_devui_model` is always
  built), `ui` (RmlUi product UI; its GPU-free part `rv_ui_model` is always built) -> `app` (composition root) ->
  `retroverse.exe`. `catalog` holds the domain, the identities, one header per port (I2 step 1), the media coverage (8)
  and the launch resolution (10); `esde` reads ES-DE's configuration and gamelists (steps 2 and 3), scans the ROM
  folders (7), resolves media (8) and translates ES-DE commands (10); `catalog` also holds the import service (11a),
  fingerprints and reconciliation (11b); `catalog_db` holds the database, the catalog store (step 4, with the import's
  writes of 11a and 11b), the queries and search (9) and the session store (12); the system probe's port is in `core`
  (`rv/core/system_probe.hpp`, ADR-0008 §2), and `rv_forbid_link(<target> EXTRA <library>...)` forbids more libraries
  per target (`cmake/LayerRules.cmake`). Code for the overlay in `rv_app` sits under `#if defined(RV_DEV_UI)`. `rv_app`
  links `rv_ui`, `rv_launcher` and `rv_platform_win` for the quick catalog's launches (I2 step A1) and, since step 13,
  `rv_app_catalog` (GPU-free and tools-free, between `rv_app_cli` and `rv_app`: the catalog worker, its
  `ui::CatalogSource`, the app's copy of the CLI's launch library, `AppCatalog` over the ES-DE or the mock catalog; on
  `rv_catalog`, `rv_esde`, `rv_catalog_db`, `rv_platform_win`, `rv_ui_model`; tested by `tests/unit/app`; since I3 step
  13r also the refresh, `library_refresh.hpp`: the import in slices, GPU-free). Since I3 `rv_app` links `rv_media` (the
  hero CRT's `ScreenService`, private, step 11) and `rv_world` (also linked by `rv_app_cli` and `rv_cli_core`);
  `rv_app_cli` holds the frame timing and phases (`frame_timing.hpp`, step 0d; the GPU passes, 17b), the interaction
  model (`world_interaction.hpp`, step 7: the world's states, input contexts and Back levels), the hero screen's policy
  (`screen_policy.hpp`, step 11) and the room's GPU-free parts: `collection_layers` (an item's front and spine through
  the cache) and `world_notice_ui.hpp` (10b), `room_items` (10c), `cover_requests` (the loader's order, the hero set's
  LRU; 12), `world_focus.hpp` (the room's side of the hover, the pull-out and Focus) and `world_label_ui.hpp` (13a),
  `world_inspect.hpp` (the held pose, the dimming, the CRT's selection; 13b), `world_card_ui.hpp` (14a),
  `world_launch_ui.hpp` (15a), `screen_room.hpp` (I4 step 6b: which room's CRT is the hero CRT), `elevator_ride.hpp`
  (I4 step 7b: the ride's state machine), `directory_data.hpp` (7b: the directory's building and games, where its
  commands lead) and `elevator_prompt_ui.hpp` (7b). `rv_app_catalog`
  holds the room's flow up to its commit: `room_snapshot` and `world_build` (10b; the building job and the floor's
  build since I4 step 5a, the building stamp since 6b), `world_reconcile` and `room_screen` (10c), `game_screens.hpp`
  (14b), `world_live_plan` (15b; room by room since I4 step 6b, `layout_difference.hpp` its comparison of a room's
  items). `rv_catalog_db`'s `catalog_stamp.hpp` (I4 step 6b) gives the catalog's stamp the building stamp holds.
  `rv_app` holds what draws: `world_session` (the flow, the commit, the notice, the lockstep wait, the records),
  `collection_instance`, `cover_loader`, `world_notice_panel` (10b), `world_start` (10c), `hero_textures` (12),
  `world_hover` and `world_label_panel` (13a), `world_inspection` (13b), `world_card` (14a), `game_screens_panel`
  (14b), `world_launch` (15a), `world_live_update` (15b), `world_ride` (I4 step 7b: the directory's commands, the
  teleports, the ride's floor swap) and `elevator_prompt_panel` (7b); the outline pass's dimming shader
  `assets/shaders/outline/outline_dim.frag` (13b). `rv_ui_model` holds `game_card_model` (14a), `media_viewer_model`,
  `game_info_model` (14b) and `quick_catalog_room.hpp` (15a); `rv_ui` `game_card_screen`, `media_viewer_screen`,
  `game_info_screen` and the vendored renderer's `rv-texture:` images (`UiSystem::setTexture`, 14a).
- `rv_world` (`src/world`, `docs/i3/`, `docs/i4/`): ids (`room:snes/fx:w03/s:3.12`; since I4 step 1 the room ids'
  four forms, `room:<system>[:<n>]`, `area:shared:<floor>[:<n>]`, `area:arcade[:<n>]`), the WorldPlan and its canonical
  JSON (`plan_json.hpp`: `writePlan`, `readPlan`, `planDigest`, `roomContentDigest`; schema 2 written, schema 1 read; a
  newer schema refused, a hand-edited plan refused; integers only: millimetres, hundredths of a degree, permille), the
  building (I4 step 1: `building.hpp`, `assets/world/building.json` and `systems.json`, `classify`, the floor layout's
  placement functions; `lift.hpp`, the schema-1 lift; `generateBuilding` and `updateBuilding`; since step 2
  `media_family.hpp`'s label quad, cover fit and family fixtures, `withFamilyFixtures`; since step 3
  `floor_layout.hpp`, a floor's derived hall, its rooms' doorways and walls on it, its areas and `roomToFloorMatrix`,
  since I6 step 2 the hall's dressing (`HallPiece`: sconces, prints, benches, plants at fixed slots),
  and `floor_scene.hpp`, a floor's scene, `loadFloorSceneData`, `toFloorSceneFile`, `floorCollectionLayout`),
  `WorldConfig` with the preset and `world.json`, the readers of
  `assets/world` (errors name `<file>:<line>` and a JSON pointer; 1a); `selectRoom` and `title_key.hpp` (the room's
  titles, `title-key/1`, representatives by region rank, status and revision, priority; 2); `generation.hpp`
  (`generate`, `loadWorldData`), `capacity.hpp`, `seeds.hpp`, the plan's lights (3); `validation.hpp` (`validatePlan`,
  37 rules since I4 step 3) and `nav_grid.hpp` (4); `pick_index.hpp` (picking, the 3-frame hover) and
  `slot_navigation.hpp` (the D-pad steps; 7); `world_store.hpp` (the world's files and lock, `savePlan`),
  `incremental.hpp` (`update`), `overrides.hpp`
  (pins: protected, never applied) and `where_is.hpp` (`PlanIndex`; 9); `scene_builder.hpp` (`toSceneFile`,
  `toSceneDescription`, `loadSceneData`) and `collection_layout.hpp` (items with transforms, pick ids, layer keys and
  flags, clusters, groups, occluders, navigation; 10a); `room_look.hpp` (the lighting profiles and the theme packs as
  data, `assets/world/lighting`, `assets/world/themes`; 16; since I6 step 3 the packs' label palettes, neon, light
  tints and `themedLighting()`, and `building.hpp`'s `themePackOf()`/`floorThemePack()` resolving a room's pack;
  since I6 step 3b the packs' `furniture`, which `scene_builder.hpp` draws in place of the hero corner's parts and the
  decoration, `drawnFixtures()` the boxes they cover). I3's other modules: `rv_geometry` the collection's
  prefabs (`display_case`, `wall_shelf`, `storage_box`, `magazine_stack`, `bookends`, `plant` (16); I6 step 3b's era
  furniture `sofa`, `beanbag`, `desk`, `office_chair`, `bar_stool`, `banker_lamp`, `showcase` and the floor lamp's
  torchiere), the cartridge's
  `spineLabel`, `shelf`'s display levels and front inset; `Slot_<level>_<index>` and `Level_<n>` sockets, `SlotFacing`,
  `slotLevels`; 1b); `rv_assets` the prefab families (`assets/manifests/families.json`, `resolvePrefab`: style §21's
  object fallbacks, one warning each; 1b) and the collection's layers (5, GPU-free, safe on the worker pool:
  `cover_fit`, `mip_chain`, `bc1`, `text_raster`,
  `collection_labels`, `rvtex`, `derived_texture_cache`); `rv_scene` the 80-byte `InstanceData` (0c), the eased
  `PoseAnimator` (7) and the static groups' layout and the outline's geometry (`instance_clusters.hpp`, 8), and since
  I4 step 3 the scene's areas (`scene_areas.hpp`: the area that holds the eye, the lights a view packs there, since
  I6 step 1 with its neighbours' seen through the doorways, each faded in and out and clipped to its doorway, and the
  environment eased between areas);
  `rv_catalog`
  `RoomEntry`, `RoomSnapshot` and `ICatalogQuery::roomEntries` (1a), which `SqliteCatalogQuery` answers in one read with
  the media left unknown (2); `rv_esde` `checkRoomMedia` and `roomMediaPaths` (`media_resolver.hpp`, 2); `rv_render`
  the texture arrays (`Texture2DArray`, `CollectionArrays`; their layout, allocator and budget in
  `rv_render_uniforms`'s `texture_array_layout.hpp`; 6), the static instance groups and the outline pass (8), a
  material's texture override (11), and in `rv_render_uniforms` the GPU memory accounting (`gpu_memory.hpp`, 0d).
  I4 step 7a: `rv_catalog` `recent_games.hpp` (the directory's favourites and recent games, their order and the
  conversion of ES-DE's local `<lastplayed>` to UTC) and `ICatalogQuery::favoritesAndRecent`, which `SqliteCatalogQuery`
  answers in one read (`src/catalog_db/src/recent_query.cpp`); `rv_ui_model` `directory_model` (the directory's
  floors, rooms, games, card and commands, from plain data the app fills) and `rv_ui` `directory_screen`
  (`assets/ui/directory.rml`).
- `tools/asset-preview`: `retroverse-asset-preview.exe` on `rv_app` and `rv_devui` (built only with `RV_DEV_UI`), and
  its GPU-free core `rv_preview_core` (`include/rv/preview/*.hpp`, namespace `rv::preview`), built in every
  configuration and tested by `tests/unit/preview`.
- `tools/cli`: `retroverse-cli.exe` and its GPU-free core `rv_cli_core` (`include/rv/cli/*.hpp`, namespace `rv::cli`:
  the command table, the dispatcher with the data-directory guard, the output contract; one `src/cmd_<name>.cpp` per
  command), built in every configuration and tested by `tests/unit/cli` and, as a process, by
  `tests/cmake/test_cli_process.cmake`.
- `tools/screenshot-diff`: `retroverse-screenshot-diff.exe` and its GPU-free core `rv_screenshot_diff_core`
  (`include/rv/screenshot_diff/*.hpp`, namespace `rv::screenshot_diff`), built in every configuration, tested by
  `tests/unit/screenshot_diff` and linked by the GPU smoke tests.
- Tests: `tests/unit/<module>/test_*.cpp` (globbed into `rv_tests`, label `unit`, no SDL or OpenGL; GLB files for
  tests come from `rv/assets/glb_writer.hpp`; `tests/unit/platform_win`, `tests/unit/launcher` and
  `tests/unit/support` start `rv_fake_emulator.exe` from the directory of `rv_tests.exe`, and `tests/unit/cli` also
  `retroverse-cli.exe` as a second process: `rv_tests` depends on both; `tests/unit/catalog_db/import_test_support.hpp`
  opens catalogs, dumps the index and state tables, scripts sources and resolvers and settles folder times,
  `settleFolderTimes()`; `tests/unit/cli/test_cli_reconciled_sessions.cpp` tests reconciliation with sessions and
  choices on the real code paths),
  `tests/platform/test_*.cpp` (globbed into `rv_platform_tests`, label `unit`: SDL platform layer, no OpenGL),
  `tests/support/*` helpers (globbed into `rv_test_support`, namespace `rv::test`: `TempDir`, `LogCapture`, `Fixture`
  (`fixture.hpp`: `tests/fixtures/<name>/` materialized into a fresh folder under `build/<preset>/test-fixtures`,
  removed afterwards; `{{FIXTURE_ROOT}}` in .xml, .json, .txt and .cfg files becomes that folder, escaped for the file;
  the fixture's `files.json` generates ROM payloads from a size and a seed, with an optional `mtime` and `hidden`,
  copies programs of the build with `copyOf` (from `testProgramsDir()`; such a copy finds its DLLs through PATH only)
  and makes empty folders, so that no payload is committed; since I3 step 1c `cover` and `video` payloads,
  `fixture_payloads.hpp`: an original picture with the title, PNG or JPEG, landscape, portrait or a cartridge, or
  truncated or not an image; a 4:3 clip of `media::writeTestVideo`, or empty, truncated, corrupt or of an unsupported
  codec; a whole generated system, `"collections"`, `fixture_collection.hpp`; and `Fixture::applyOverlay(folder)`, a
  materialized folder laid over the root), `writeProgramStub` (`program_stub.hpp`), `runChild`
  (`child_process.hpp`: a program run to completion, its command line built by `launcher::buildCommandLine`, stdout and
  stderr in files of the test's choosing, apart or together, a timeout), `FakeSystemProbe` (`fake_system_probe.hpp`: a
  machine as maps of registry values, files and folders), `Junction`, `ListingDenied`, `DataReadDenied`,
  `writeFileAnyLength` (`file_system.hpp`: junctions without admin rights, a folder this user cannot list, a file whose
  data cannot be read, paths past MAX_PATH), `ScopedLaunchAllowList`, `ScopedEnvironmentVariable`, `ScopedPathPrefix`
  (`scoped_environment.hpp`: tests that launch set `RV_LAUNCH_ALLOWLIST` for their own duration, then put it back),
  `ensureBackgroundLaunches`, `prepareAutomatedRun` (`background_launches.hpp`: `RV_LAUNCH_BACKGROUND=1` unless set,
  and no failure dialogs; `rv_tests` and `rv_smoke_tests` call it before any test),
  `generateCatalogDatabase` (`catalog_generator.hpp`: a deterministic catalog database of any size, written in one
  import transaction with the search index suspended; `rv_test_support` links `rv_catalog` and `rv_catalog_db`)),
  `tests/fixtures/<name>/` (original, minimal fixtures; text committed as LF, so CRLF, BOM and oversized files are
  written at test time; `tests/fixtures/esde/standard` is the ES-DE installation the catalog steps share: its README
  lists what each part exercises and the counts they assert, and it only ever grows additively; a step that needs
  another situation adds a fixture folder of its own: `esde/portable` (a portable installation, `portable.txt` variants
  written into the copy), `esde/custom-dirs` (ROM and media roots and system folders moved with `~` and `%ESPATH%`),
  `esde/custom-systems` (the user's `custom_systems` with ES-DE's quirks), `esde/nested-roms`, `esde/multi-disc`,
  `esde/arcade`, `esde/noload` and `esde/long-path` (the ROM scan: namesakes, game directories and gamelist entries in
  sub-folders, disc groups, MAME sets and names, `noload.txt`, hidden folders and `ParseGamelistOnly`, paths past
  MAX_PATH), `esde/media-roots` (an empty media root; a missing one and one on an unmapped drive as variants), each
  README stating its expectations; their settings hold fake scraper credentials that nothing may show, log or
  record; for I3 `esde/snes-room` (50 invented titles with generated covers and clips, damaged ones among them, G10's
  ten in `additions/`), `esde/snes-single` (one ROM, no media) and `esde/snes-stress` (2,000 generated titles, 300
  second releases), their system `snes` with the invented full name "Lumen 16 Home System", their READMEs stating the
  counts the tests assert; for I4 `esde/building` (step 2: nine systems with invented full names over the ground
  floor and four floors, every family's covers, a custom system, an excluded one, `additions/` with a new system); `tests/fixtures/world/` the golden selections, plans and scenes), `tests/fake_emulator`
  (`rv_fake_emulator`, built with the tests or the spikes), `tests/collection_probe` (`rv_collection_probe`, built with
  the tests; its `probe_layout.hpp` shared with the smoke tests), `tests/ui_probe` (`rv_ui_probe`, built with the
  tests, I4 step 7a: the directory from a fixed model; its `directory_fixture.hpp` shared with the unit and smoke
  tests),
  `tests/input_scripts` (scripted verification runs),
  `tests/smoke/*.cpp` GPU smoke tests (globbed into `rv_smoke_tests`, label `gpu`: GPU-free itself, it starts the
  executables of its build with `tests/support/child_process.hpp` and reads their run reports and screenshots;
  `smoke_support.hpp` runs them, `image_checks.hpp` measures screenshots, `launch_support.hpp` prepares the launches)
  with their test scenes in `tests/smoke/scenes` and mock catalogs in `tests/smoke/catalogs`,
  `tests/cmake/test_*.cmake` (label `unit`, listed in `tests/cmake/CMakeLists.txt`, run by ctest in CMake script mode:
  the build's CMake scripts, e.g. the Release asset mirror, `retroverse-cli` as a process, its exit codes with
  stdout and stderr apart, and the layer rules, `unit.cmake/layer_rules`).
- Configuration files, JSON with comments: shipped defaults under the asset root, the user's under
  `<user data>/config`, which Retroverse reads and never writes: `assets/config/default_bindings.json` under
  `bindings.json` (per action), `assets/launcher/emulators.json` (the emulator profiles: one RetroArch profile for 50
  systems, `rv/launcher/emulator_profile.hpp`) under `emulators.json` (a user profile replaces the shipped one with its
  id whole; a broken user file or profile is left out with an issue; `"systemChoices"` gives Retroverse's choice for a
  system, `{"snes": {"command": "bsnes"}, "gb": {"profile": "retroarch"}}`: one of the system's ES-DE command labels,
  compared exactly, or a profile id, the user's replacing the shipped one per system; the shipped file sets none, and a
  stale label or unknown profile makes the system "requires launcher configuration"), and since I3
  `assets/world/presets/<id>.json` under `world.json` (per key: `preset` names another preset, `seed` a seed or null,
  integers only, millimetres and permille, since I4 step 4 `sharedRoomMinTitles`, the distinct titles a system entering
  the building needs for a room of its own, 30, 0 giving every system one; a broken file is left out whole with one
  issue). The world's data, read
  strictly (unknown or duplicate keys and fractions refused with their line): `assets/world/templates/` (the room
  template `console_room_9x10.json`), `platforms/`, `media_families.json`, `lighting/` (the profiles `evening` and
  `afternoon`), `themes/` (the room's materials); prefab families in
  `assets/manifests/families.json`. The one exception,
  `library.json` (the library's id, the ES-DE home and install, root overrides), is written only by `retroverse-cli
  library create`; choices made later go to the database (ADR-0008 §6).
- `third_party/glad` is generated and vendored (see its README); do not edit it by hand.

