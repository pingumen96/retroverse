# I11 plan: the consoles' real identity, and every emulator in VR

This file holds the I11 section, already pasted into `docs/roadmap.md`, the row for the iteration table, the order of
the steps, and the decisions made with the user. The decisions themselves are
[ADR-0019](../decisions/ADR-0019-console-identity-as-user-content.md) and
[ADR-0020](../decisions/ADR-0020-every-emulator-in-vr.md).

## Iteration table row

| I11 | The consoles' real identity and every emulator in VR (ADR-0019, ADR-0020): the real names, logos, colours and a history plaque per room read at runtime from ES-DE's themes; real console models from the user's content folder; every emulator of the library and the Windows games played on the CRT in the headset | Gate I11: `gate.ps1 -Gate I11`, then a headset session with one game per emulator |

## Decisions made with the user (2026-11-07)

The user's request: "tutti gli emulatori devono funzionare in VR; devi usare i loghi originali e i nomi veri delle
console; devi usare i modelli veri delle console nelle varie stanze, se serve cerca su internet", with proposals for
the next iteration. Their answers in planning:
- **Where trademarked content lives:** local, outside the repository. Names, logos, descriptions and colours are read at
  runtime from ES-DE's themes. Models live in `<user data>/content/consoles/`, and their sources in a private sibling
  repository, `retroverse-content`. `assets/`, fixtures, goldens and I9's package stay brand-free.
- **Where models come from:** mixed. A free model with a clear licence where a good one exists, normalized by a Blender
  script; a Blender model from real measurements where none does; the generic prefab as the fallback.
- **VR's scope:** every emulator and the Windows games (shortcuts, Steam), with exclusive fullscreen detected and
  explained.
- **Extras chosen:** a history plaque per room; the systems' colours on the neon and the portal.
- **Offered and parked:** the insertion in VR; the real controllers beside the console.

Decisions taken on the user's behalf:
- I11 starts after I7's Gate Pad; this step 0 only writes documents, on its own branch.
- The world plan never changes. Identity and models are applied when the scene is built, so every committed golden
  stays 0 pixels apart, and one golden (G22) is added, on an invented fixture theme.
- `linear-es-de` from the ES-DE install is the first theme read (complete metadata in Italian and SVG logos for all 45
  systems), before the active theme (`coinops-es-de`, mostly PNG).
- The theme's `systemName` ("Super Nintendo") is the display name. ES-DE's fullname ("Nintendo SNES (Super Nintendo)")
  becomes the detail line.
- lunasvg for SVG, inside `rv_assets` only. WebP is parked.
- Model and logo textures are compressed with `stb_dxt` (BC1, BC3, BC5) and cached, because the busiest floor was at
  477.5 of its 512 MiB.

## Section for the roadmap

The text below is the roadmap's I11 section as pasted.

## I11: the consoles' real identity, and every emulator in VR

[ADR-0019](../decisions/ADR-0019-console-identity-as-user-content.md) and
[ADR-0020](../decisions/ADR-0020-every-emulator-in-vr.md), planned with the user on November 7, 2026 (the decisions:
[i11/plan.md](plan.md)):
- **Identity as user content:** real names, logos, colours and a history plaque, read at runtime from ES-DE's themes,
  never copied into the repository.
- **Real console models** from `<user data>/content/consoles/`, normalized in Blender, the procedural console the
  fallback.
- **Every emulator in VR:** launch profiles for windowed play, the captured window followed, shortcuts and Windows
  games, exclusive fullscreen detected, stop watching, the large screen.

The world plan never changes. Every committed golden stays 0 pixels apart, and G22 is added on an invented fixture
theme. Real names, logos and models are checked only through `real-library.ps1` and by eye, their screenshots kept
under `build/`. Step notes are in `docs/i11/`.

| # | Step | Needs | Proof | Status |
|---|---|---|---|---|
| 0 | ADR-0019, ADR-0020, this plan; the amendments (style §1, §7.3, §17; specification §19.4; ADR-0001, 0006, 0008, 0010, 0013, 0014, 0017; CLAUDE.md, `docs/reference/safety.md`); the BACKLOG's items moved here | | The files exist | done: the documents; `retroverse-content` created (README, `SOURCES.md` with the first candidates) |
| 1 | The identity reader: `esde::themeSystemMetadata()`, `esde::themeLogo()` (the layouts of ADR-0019 §2, .svg, .png, .jpg), `Theme` in the settings whitelist, `app::SystemIdentity` on the worker with each field's source, `identity.json`, `retroverse-cli identity show [--system]` | 0 | Unit on `tests/fixtures/esde/theme` (invented systems, original SVG shapes): each field and its source, the order, hostile XML and SVG, the limits, names through `utf8.hpp`. Real library: `identity show` lists the 45 systems with name, logo and description, 0 differences | done ([step-1.md](step-1.md)): `catalog::SystemIdentity` resolved in `rv_esde` (`themes.hpp`, `identity.hpp`), the fixture `esde/theme`; the real library's 195 systems all named by linear-es-de with an SVG logo, an Italian description and colours, 0.68 s, 0 differences |
| 2 | Logos as textures: lunasvg in `rv_assets` (`assets::rasterizeSvg`, its bounds), rasterized on the pool, BC3 through `stb_dxt`, cached in `cache/identity/`, uploaded through the derived-texture path; `rv-texture:logo:<system>` | 1 | Unit: fixture SVGs against reference PNGs, a hostile SVG refused within its bounds, the cache key. GPU: the logos in a UI probe | done ([step-2.md](step-2.md)): `assets::rasterizeSvg()` and `loadLogo()` with their bounds and refusals, `resampleSrgbAlpha()`, lunasvg 3.5.0; checked at known pixels instead of reference PNGs; the cache and BC3 move to step 4, the GPU proof to step 3 |
| 3 | Real names in the UI: the identity's name wherever a system's name shows, ES-DE's fullname as the detail; logos in the directory's room rows and the elevator panel, VR's panels included | 2 | Unit: the name's choice. GPU: the directory and the card with the fixture identity; every golden unchanged without a theme | done ([step-3.md](step-3.md)): the identity resolved on the catalog worker with the systems (identity.json, the themes); its name in the chips, rows, card, directory, statistics, coverage, library, notices, edit bar and regenerate preview, ES-DE's name on the emulators screen; the console rooms' logos in the directory (`SystemLogos`: rasterized on the pool, `rv-texture:logo-<system>`, GPU kind `uiTextures`); unit and GPU on the invented theme fixture |
| 4 | Logos in the world: the neon (the logo emissive, tinted), the portal, the blade, the fascia; the name under it in TrueType (`stb_truetype`, Atkinson Hyperlegible Bold, UTF-8); the generic bitmap sign as the fallback; shared rooms' sections on the portal; an expansion's number | 2 | Unit: the sign's layout per case. GPU: the fixture room; **G22**; G01 to G21 0 pixels apart. By hand: the real building (`--start-room room:snes`), screenshots under `build/` | done ([step-4.md](step-4.md)): `assets::makeIdentitySign()` (the logo alone, the name in Atkinson Hyperlegible Bold for a system without one; lit plates in the logo's colours, the neon's tubes along its outline and colour edges); `WorldSigns` puts them on the scenes' sign materials at runtime (texture overrides, GPU kind `signTextures`), the scene files and plans unchanged; console rooms' neon, portals, blades and fascia, shared rooms' hall signs with their sections' logos; **G22**; the cache and BC3 not needed yet |
| 5 | The history plaque: `room_plaque.rml` through `renderTo` on a brass plaque beside the hero corner (placed at scene build), Focus and Inspect, VR | 4 | GPU: the plaque's text from the fixture, Inspect on it, on the fake XR runtime | done ([step-5.md](step-5.md)): the identity's maker, year, type and description on `CatalogSystem`; `assets::makeHistoryPlaque()` (brass, the logo engraved, the details, the description wrapped and cut); `WorldPlaques` hangs `plaque:<room>` beside the hero corner at runtime (`replaceObjects`, the plan and scene files unchanged) and puts the plate on it; the ray labels it, interact opens `room_plaque.rml`'s card, VR too; G22 rewritten; drawn GPU-free, not through `renderTo` |
| 6 | The systems' colours: the palette's accent clamped to style §3's band, on the neon and the portal's fascia; Settings "Colori dei sistemi" (`world.systemColors`) | 4 | Unit: the clamp's table. GPU: the fixture room tinted; with the setting off, 0 pixels from the untinted image | done ([step-6.md](step-6.md)): the identity's palette in `CatalogSystem::colors`; the accent the entry of the largest chroma, clamped in HSV into the packs' neons' band (`systemAccent()`); the console rooms' neon materials, the hub's fascia, the neon's glow and picture light tinted at runtime (`WorldSigns::tint()`); `world.systemColors` in world.json, settings.json and Impostazioni, never in the digest; G22 rewritten with the colour, and checked off against G02 |
| 7 | Console models in the room: `Paths::contentDir()`, `content:` paths, `console.json` read as a user manifest, ADR-0019 §7's validation and budgets (textures BC1, BC3, BC5 through `stb_dxt`, cached), the hero corner's console drawn from the model at scene build, its collider, showcases, model sockets in `consoleSocketOf()`, the family's procedural fallback in place of the magenta cube; `retroverse-cli content add-console`, `content check`, `content list`; the asset preview on a content folder | 0 | Unit: the manifest's rules and refusals, socket derivation, an oversized model refused, a missing file falling back. GPU: an original fixture console (`make_test_models.py`) in the fixture room, the insertion into its socket, its collider | done ([step-7.md](step-7.md)): `content/consoles/<system>/` (`console.glb`, `console.json`); `assets::checkConsoleModel()` (sides, footprint, `scaleCm`, triangles, 2 MiB compressed); textures to BC1/BC3 with mips (`stb_dxt`), `Texture2D::createCompressed()`, GPU kind `contentTextures`; `WorldConsoles` swaps the model in at runtime in the procedural console's place, its collider and media socket (the insertion), a failing model never shown; `retroverse-cli content add-console/check/list`; the asset preview opens a content folder; the fixture console made in C++; no disk cache, normal maps BC1, the memory row with steps 9-10 |
| 8 | The pipeline in `retroverse-content`: `normalize_console.py` (Blender 4.3 in the background) and `author/<system>.py`, `dimensions.json` | 7 | One downloaded and one authored console pass `content check` and stand in their rooms | done ([step-8.md](step-8.md)): in `retroverse-content` (9f4ece1): `dimensions.json`; `normalize_console.py` (imports glb/gltf/fbx/obj/blend, front, scale to the measurements, origin, decimation, Cycles bake of a 1024 base colour and 512 normal and ORM maps, anchors, a JSON summary); `author/_kit.py` and `author/snes.py` (the SNES from measurements, 1054 triangles); `tests/run_tests.ps1` checks every result with `retroverse-cli content`; the SNES installed and shown on the real snes room's cabinet; the downloaded half proven on an invented messy model (Sketchfab needs the user's login) |
| 9 | Batch 1, the 16 home consoles with rooms of their own on the reference library (nes, fds, snes, megadrive, mastersystem, psx, ps2, gc, dreamcast, atari2600, atari5200, atari7800, atarijaguar, colecovision, intellivision, odyssey2): candidates found and recorded in `SOURCES.md`, downloaded by the user, normalized or authored, installed | 8 | `content check` for all 16; each real room seen by eye against `example.png` and style.md | done ([step-9.md](step-9.md)): twelve of the sixteen authored in `retroverse-content` from `dimensions.json` (nes, megadrive, mastersystem, psx, ps2, gc, dreamcast, atari2600, atari7800, intellivision, colecovision beside the snes), built and checked by eye in a live Blender 5.2 through its MCP server, the kit's media sockets turned the way they face (the NES's door and the PS2's tray to the front, the Intellivision's slot to the right); `content check` passes all twelve in the private real-library data directory, each shown on its room's cabinet; the Sketchfab candidates kept as replacements (the user's login); fds, atari5200, atarijaguar and odyssey2 with step 10 |
| 10 | Batches 2 to 4: the 7 handhelds (gb, gbc, gba, nds, atarilynx, virtualboy, ngpc), the 6 computers (c64, amiga, amstradcpc, apple2, atari800, atarist; their place decided after measuring), the shared rooms' 15 systems in their sections' showcases | 9 | As step 9; the busiest floor at most 512 MiB (else the residency of ADR-0019 §9) | |
| 11 | The window followed: the scorer, the choice made again every 100 ms, the capture restarted on a new or closed window, foreground retried, black frames watched | 0 | Unit: the scorer's table. GPU with the fake's modes `splash`, `two-windows`, `recreate`, `child`, `late`: the CRT shows the game window, its counter advancing | done ([step-11.md](step-11.md)): `launcher::chooseGameWindow()` and `WindowFollower` (ranks, 300 ms hysteresis), `WindowChanged`, the capture restarted and retried, black reported after 5 s; the runner's modes `splash`, `two-windows`, `recreate`, late; GPU: the CRT's last picture the game's colour after a splash and beside a main window, a black window reported; `child` left to step 13's detached sessions |
| 12 | VR launch profiles: `emulators.json` schema 2 (`vr`: arguments, the append-file, window rules, `inGame`), the profiles of ADR-0020 §1 checked against each program's help, ES-DE commands matched by executable, `retroverse-cli launch --dry-run --vr` | 11 | Unit: argv with and without VR on ES-DE 3.4.1's commands, the append-file's bytes, a fixture config never opened for writing. GPU: the fake reports the VR arguments. Real library: `profiles` lists the standalone emulators found | done ([step-12.md](step-12.md)): `emulators.json` schema 2 with `programs` (files, window rules, `vr`: arguments added and dropped, the append-file, the load), `launcher::adaptLaunch()` on every launch, the shipped programs (RetroArch, Dolphin, PCSX2, DuckStation, RPCS3, AppleWin, Altirra, sdl2trs; the first four and AppleWin read from the user's builds), `launch --dry-run --vr`, `profiles` with the programs in use; GPU: the fake gets its program's VR arguments and append-file, the CRT shows the colour the file set; the VR block moved from profiles to programs |
| 13 | Shortcuts and Windows games: "open the game's file" translated natively (`.lnk`, `.exe`, `.url` through a listed protocol), `FollowWindow`, `%RUNINBACKGROUND%`, exclusive fullscreen detected and its notice | 11 | Unit: `.lnk` reading on shortcuts made at test time, the scheme list, `.bat` still refused, the detached choice. GPU: a `.lnk` to the fake, the `detached` mode followed to its end, the notice on `black` | done ([step-13.md](step-13.md)): `ISystemProbe::readShortcut()` (IShellLinkW, never resolved), "open the game's file" in the translation (`.lnk`, `.exe`, `.url` of emulators.json's `protocols`; anything else refused), `%RUNINBACKGROUND%` and activations as `FollowWindow` (processes adopted by their windows and their ancestry), activations refused in automated runs, exclusive fullscreen from the shell's state or black frames, the VR notice; the real `.lnk` and the detached game launched through `retroverse-cli` (the fake), GPU: the notice before the CRT |
| 14 | Watching from VR: stop watching (the menu button held), "Torna al gioco" and "Il gioco è terminato" on the card, the large screen, the audio endpoint's notice | 11 | Unit: the command's states. GPU on the fake runtime: stop and resume, the large screen from the lobby, the notice on a fake mismatch || done ([step-14.md](step-14.md)): the left menu button held 1 s stops watching a game in VR (the session panel: "Torna al gioco", "Il gioco è terminato"; the capture ends, the screens play again, the controllers drive the panel), watching again captures it anew; the large screen (2.4 m, 2.8 m ahead, panel pass with a crop and sRGB encoding) where no CRT takes the game or with xr.json's `gameScreen` "large"; the headset's audio endpoint (XR_OCULUS_audio_device_guid) against Windows' default, an 8 s notice when they differ; launch and xr records |
| 15 | Gate I11: `scripts/gate.ps1 -Gate I11` (and `-RealLibrary`), `docs/testing/gate-i11.md` with the emulator matrix | 1-14 | The gate passes; the user's headset session with one game per emulator (RetroArch in its real configuration, Dolphin, PCSX2, RPCS3, Altirra, AppleWin, sdl2trs, a Windows game from a shortcut and from Steam), the rooms' logos, plaques and models; the user declares I11 done | |

Order: 0, then steps 1 to 6 beside 11 to 14 (parallel worktrees: each part owns its modules); 7 once 2 is in; 9 and 10
while the rest goes on (they wait on downloads and Blender); then 15.

### Gate I11

`scripts/gate.ps1 -Gate I11` runs, under `scripts/focus_watch.py` with `RV_LAUNCH_ALLOWLIST` naming the build's fake
emulator:
- what Gate Pad runs;
- G22;
- a row per I11 area counted from those runs: the identity, the logos, the signs, the plaque, the colours, the models,
  the window, the profiles, the shortcuts, watching from VR;
- the VR benchmark path in Release with the fake emulator captured, in each of the `inGame` profiles.

It prints the summary table (logs in `build/gate-i11`) and exits 1 when a step failed.

`-RealLibrary` adds one `scripts/real-library.ps1` run, with ES-DE closed, `RV_AUDIO=null`, read-only snapshots before
and after and 0 differences:
- `identity show`;
- `content check` on the user's content folder;
- every real collection room started with its logo and model;
- the busiest floor within 512 MiB;
- nothing launched.

`docs/testing/gate-i11.md` holds:
- the figures;
- the emulator matrix (each program, its VR arguments, its window rule, the user's result);
- what to try.
