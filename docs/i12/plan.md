# I12 plan: the rooms' density

This file holds the I12 section, already pasted into `docs/roadmap.md`, the row for the iteration table, the order of
the steps, and the decisions made with the user. The decisions themselves are
[ADR-0021](../decisions/ADR-0021-the-rooms-density.md).

## Iteration table row

| I12 | The rooms' density (ADR-0021): the shell (skirting, rail, wallpaper, cornice, beams, pendants), a gallery of the user's own art, a lived-in hero corner with the last game played, full side walls, the shelves' tabs and captions; dust in the light, the neon's flicker and the screen's glow, synthesized room tones and footsteps, a fan, a clock, LEDs; the GPU memory budget at 4 GiB | Gate I12: `gate.ps1 -Gate I12`, then the user walks a room of each pack beside its concept panel |

## Decisions made with the user (2026-11-08)

The user's request: "vorrei migliorare brutalmente la qualità delle singole stanze, rendendole più dense e più
immersive. Fai proposte e le valutiamo assieme". Twelve proposals were presented with their cost and risk, from the gap
Gate D left against the concept (`docs/testing/gate-d.md`): bare upper walls and ceilings, bare side walls in rooms of a
few hundred titles, an empty floor, no colour from art, a diagram of a hero corner, nothing moving or sounding. The
user's answers:
- **Structure and walls:** the shell (P1), the gallery of the user's art (P2), full side walls (P4), the surfaces (P12).
- **Objects and details:** the hero corner as a lived-in set (P3), the shelves' tabs and captions (P5), life (P8). The
  arcade's and the handhelds' specifics (P11) were not chosen.
- **Atmosphere and technique:** dust and light (P6), the sound of the room (P7). Reflections (P10) and compact rooms
  (P9) were not chosen.
- **Order:** as the eye reads a room from its doorway.
- **The memory budget**, at the plan's review: "vorrei solo che valutassi 4gb di vram come budget d'ora in poi". The
  roadmap's GPU memory rows are 4 GiB from now on (ADR-0021 §8).

Decisions taken on the user's behalf:
- I12 gets its own ADR and plan and runs in its own worktree (`E:\Progetti\retroverse-i12`), beside I11's steps 9, 10
  and 15, which wait on the user's model downloads. Its steps branch as `i12/step-<n>`, each on the previous, and are
  merged into main when the user asks.
- The world plan never changes, but for the density default of new and regenerated worlds; stored worlds keep 0.4.
- The shell's details are dressing primitives, never new `room_shell` roles; one look pendant light per base room
  (a real room draws 15 of its 16 lights already); picture lights are hoods and washes, never lights.
- The pads lie on the media rack and on the rug, never on the cabinet: its top is the CRT's and the console model's.
- The user's media stay runtime textures; fixtures and goldens use invented media (a gallery overlay of the
  `snes-room` fixture, applied by the gallery's smoke test alone).
- One items-file schema bump (3, at step 2) carries the gallery's media paths and step 5's sort key, developer,
  publisher and genre, so the items files are rewritten once.
- The gallery's kinds by pack: the arcade lightboxes only, the lounges fanart first, the study screenshots first, the
  others by weights (poster 0.4, frame 0.3, portrait 0.2, lightbox 0.1).
- Sounds are synthesized, never sampled; footsteps are kept in VR, lower, and stop while teleporting.
- Dust in every room's pack; none in the hub, the halls or the test room. Every motion is frozen under
  `reduceMotion` and deterministic under `--fixed-dt`.
- The last game played is the room's game with Retroverse's latest session (the items file's `lastPlayed`); ES-DE's
  `<lastplayed>` is not in the room's snapshot.
- Goldens are regenerated on purpose at the steps that change their views; none is added. (G01 and G21 were to move
  at step 1 with the test room's armchair; the step gave the packs' seats woven materials of their own instead, so
  they stay: ADR-0021 §7 as amended.)
- A gate step closes the iteration, as every iteration's.
- Offered with the budget, not chosen: cover arrays at twice the resolution (BACKLOG, "Parked at I12's planning").

## Section for the roadmap

The text below is the roadmap's I12 section as pasted.

## I12: the rooms' density

[ADR-0021](../decisions/ADR-0021-the-rooms-density.md), planned with the user on November 8, 2026 (the decisions:
[i12/plan.md](plan.md)): every single room denser and more immersive, closer to the concept's panels 2, 4, 5 and 6.
- **The static look**, in the order the eye reads a room from its doorway: the shell (skirting, rail, wallpaper,
  cornice, beams, pendants), a gallery of the user's own art, a lived-in hero corner with the last game played, full
  side walls, the shelves' tabs and captions.
- **Atmosphere, sound and life:** dust in the light, the neon's flicker and the screen's glow, synthesized room tones,
  footsteps and clicks, a fan, a clock, LEDs.
- **The GPU memory budget** is 4 GiB from November 8, 2026 (ADR-0021 §8).

The world plan never changes, but for the density default of new and regenerated worlds; every piece is chosen at
scene build from the room's seed or swapped in at runtime. Goldens are regenerated on purpose at the steps that change
their views; none is added. The real library's rooms are checked through `real-library.ps1` and by eye, their
screenshots kept under `build/`. Step notes are in `docs/i12/`.

| # | Step | Needs | Proof | Status |
|---|---|---|---|---|
| 0 | ADR-0021, this plan; the amendments (ADR-0010 §2, ADR-0013's memory, ADR-0014 §1, ADR-0016 §9 and §12, ADR-0018 §2, ADR-0019 §1 and §9; style §6.3 and §25); the BACKLOG's items moved here and the proposals parked; the GPU memory rows, the tests' limits and `gate.ps1` at 4 GiB | | The files exist; the unit tests pass; `gate.ps1` parses and its memory rows read 4 GiB | done: the documents; the four tests and `gate.ps1` at 4 GiB |
| 1 | The shell: a picture rail at 2.7 m over the doorway's casing, the pack's damask from the wainscot to the rail, a cornice and beams every 3 m as the room's dressing (no `room_shell` role, no plan change); a drum near the doorway, lit by one look light after the accents in a slot they leave free, and spot cans over the display cases; the lounge's blue cove; procedural damask, herringbone parquet, fabric weave and leather on the packs' floors and seats (ADR-0021 §1 as amended by the step) | 0 | Unit: the surfaces (sizes, tiling, determinism, unit normals); the reader's keys and refusals; on every golden plan room the bands against their walls and around the casing, the beams, the lamps' places and parts, the look light last within 16, no accent displaced. GPU: room:snes from the doorway: the bands, beams and lamps in the scene, the look light packed, the paper never black, the drums glowing. The room goldens regenerated on purpose; the benchmark and VR rows; the busiest real floor against 4 GiB | done: the shell in the six room packs; fifteen goldens regenerated, G01, G11, G14, G20 and G21 unchanged; benchmark p99 4.03 ms, VR p50 7.33 ms at 72 Hz; the busiest real floor 541.0 MiB ([i12/step-1.md](step-1.md)) |
| 2 | The gallery: framed pictures of the room's own games over the modules (fanart posters, 3D-box or cover portraits, marquee lightboxes, screenshots on a mat; never stretched), a hood and its wash over every other frame; the items file's schema 3 (the gallery's media, the caption cards' texts); `WorldGallery` reads, fits and BC1-compresses them on the pool, resident for the floor (`galleryTextures`); `world.gallery` in Impostazioni (ADR-0021 §2 as amended by the step: the low pieces' frames with step 4) | 1 | Unit: the frames per golden plan room, the chooser (priority, seed, a game once, the print without media), the fit, schema 3 and the older schemas. GPU: `snes-room`: the frames, the pictures shown, none failed, their bytes; off, the prints. Room goldens regenerated | done: frames over every module, the real library's media in them (the arcade's marquees lit); schema 3; fifteen goldens regenerated; benchmark p99 5.16 ms, VR p50 7.51 ms; the busiest real floor 606.9 MiB; the real library's timing rows to run again on a quiet machine ([i12/step-2.md](step-2.md)) |
| 3 | The hero corner as a lived-in set: speakers, a media rack with LEDs, pads with cables on the rack and the rug, a coffee table with magazines and an open box, a side table and lamp, a cushion, per pack (`furniture.heroDressing`, inside the corner, the passage and the CRT's line of sight kept); the game played last in the console's slot, a runtime proxy as the hub showcase's | 1 | Unit: six prefabs; every pack's set inside the corner on the golden plans with no new finding, clear of the console's reserve and the plaque; the proxy's choice and pose. GPU: after a launch returns, its game stands in the room's slot, a content model's too | |
| 4 | Full side walls: tall decoration variants in every pack (a bookcase of magazines and manuals, a record or tape rack, a vitrine of controllers, a tower speaker, a coat stand, a pegboard of pads, palms and ficus) in a rhythm (no two alike side by side, heights alternating); the density default 0.65 for new and regenerated worlds | 3 | Unit: four prefabs and the plant kinds inside their sockets and under the ceiling; the rhythm; the default; a stored plan at 0.4 updated without a change or a finding. GPU: `snes-stress` at 0.65, three tall pieces per side wall. Goldens regenerated with step 3's | |
| 5 | Shelf detail: letter tabs on the spine-out levels, caption cards under the display cases' titles (title; year and developer), a section sign over each section of a shared room; atlases rasterized on the pool, following live updates and the editor | 2, 4 | Unit: the runs, cards and sites on synthetic shelves; the atlases; ids the editor never picks. GPU: `snes-stress` up close, the tabs and cards counted and legible; the shared handheld room's two signs; a moved module's tabs following. G13 regenerated, the others where their cards pass the tolerance | |
| 6 | Light and atmosphere: dust motes in the key light's cone and the pendants' pools, faint light cones in the lounge and the arcade (an atmosphere pass after the blended batches, its motion on the GPU from the simulation time), the neon's flicker, the CRT's spill light taking the picture's average colour | 1 | Unit: the emitters, the motes' formula, no flicker dip in the first 2 s, the uniform blocks. GPU: particles drawn, frozen under `reduceMotion`, cones in the lounge, the pass in the report; G01 and G21 exact; the room goldens regenerated once; the benchmark and VR rows | |
| 7 | The sound of the room: synthesized room tones per pack (the TV's hum and hiss, the neon's buzz, the arcade's murmur, the study's clock), footsteps by surface, the pull-out's clack, the case, the insertion's click, the menus' tick; the `ambience` and `effects` buses and their rows in Impostazioni | 0 | Unit: every sound's length, peak, loop and determinism; the mixer's loops and one-shots; the cadence and the surfaces. GPU (null audio, captured): six steps in a 2 s walk, the ambience silent at volume 0, a clack on a pull-out, the duck | |
| 8 | Life: a ceiling fan and a lava lamp (the 90s collector), a wall clock with the real time (the study), blinking LEDs, static on the secondary TV between clips; still under `reduceMotion`, 10:10 in frame-limited runs | 6 | Unit: four prefabs, the hands' angles, the frozen flags. GPU: the fan's disc moves between two frames and stands still under `reduceMotion`; the secondary TV's static between clips, never on the hero | |
| 9 | Gate I12: `scripts/gate.ps1 -Gate I12` (and `-RealLibrary`), `docs/testing/gate-i12.md` | 1-8 | The gate passes; the user walks a room of each pack beside its concept panel and declares I12 done | |

Order: 0, 1, then 2 beside 3 (2 owns the gallery and the items file, 3 the hero corner), 4 after 3, 5 after 2 and 4,
6 after 1, 7 beside any of them in a worktree of its own (it owns `rv_audio` and the app's sound), 8 after 6, then 9.

### Gate I12

`scripts/gate.ps1 -Gate I12` runs, under `scripts/focus_watch.py` with `RV_LAUNCH_ALLOWLIST` naming the build's fake
emulator:
- what Gate D runs: the build matrix, the unit and GPU tests in Debug and Release, every golden, the benchmark path;
- G21 and the VR benchmark path;
- a row per I12 area counted from those runs: the shell, the gallery, the hero corner, the walls, the shelf detail, the
  atmosphere, the sound, the life;
- Gate D's measures on the room goldens (median luma, top quarter, colour and accent shares) beside the concept
  panels'.

`-RealLibrary` adds one `scripts/real-library.ps1` run, with ES-DE closed, `RV_AUDIO=null`, read-only snapshots before
and after and 0 differences:
- the busiest floor with two screens, the gallery and the atlases within 4 GiB;
- a room of each pack shown, its screenshots under `build/gate-i12` for the user's eyes;
- nothing launched.

It prints the summary table (logs in `build/gate-i12`) and exits 1 when a step failed. `docs/testing/gate-i12.md` holds
the figures, the measures beside the panels and what to try.
