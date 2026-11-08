# I11 step 9: the home consoles, authored

ADR-0019 §7; roadmap I11 step 9. Step 8 built the pipeline and one console. This step fills the home consoles'
rooms: twelve of batch 1's sixteen systems now have a model in `<user data>/content/consoles/<system>/`, built in
Blender from real measurements. The work lives in the private sibling repository `retroverse-content`
(`E:\Progetti\retroverse-content`, commit after 9f4ece1), never in this one; nothing in Retroverse's code changed.

## What was built (in retroverse-content)

- **Eleven more authored consoles**, beside step 8's snes: nes, megadrive, mastersystem, psx, ps2, gc, dreamcast,
  atari2600, atari7800, intellivision, colecovision (`author/<system>.py`). Each is the console's shape simplified
  for a cabinet seen a metre or two away, in plain materials with sRGB colours, no logo and no lettering:
  1,000 to 3,500 triangles, no textures, 24 to 64 KiB each.
  - The NES's two-level body with the door's lid and the red buttons; the Mega Drive's ridge around the slot and its
    medallion; the Master System's red line; the PlayStation's and Dreamcast's round lids; the PS2 lying flat with its
    ribbed top and tray bezel; the GameCube with its lid, ports and handle; the Atari 2600's woodgrain panel and
    four switches; the 7800's silver plate on the slope; the Intellivision's and ColecoVision's controllers in their
    wells.
- **`dimensions.json`** holds the twelve systems' width, height and depth in centimetres with the source of each
  (museum catalogues, manufacturers' specifications, dimensions.com; checked 2026-11-08), and the medium's opening:
  `slot` (a cartridge) or `tray` (a disc lid or tray), its middle in Retroverse's axes, its size, and `facing`.
- **The kit (`author/_kit.py`)** gains wedges (a sloped top), cylinders, discs, `clear()` for a live Blender (its
  factory reset would unload the add-ons), the export's summary (triangles, size, bottom), and **media sockets turned
  the way they face**: `up` (the default), `front` (the NES's door, the PS2's tray), `right` (the Intellivision's side
  slot), `left`. The socket's frame follows step 7's contract (+Y out of the opening, +Z where the label faces): the
  NES's cartridge lies half in through the front, label up; the Intellivision's goes in from the right. A disc lid's
  socket is a disc in the lid's colour, so that it does not show on the lid.
- **The consoles were authored in a live Blender 5.2.3 through its MCP server**, each script loaded by its path,
  built, looked at from the front and three quarters and corrected, then exported; the scripts still run under
  Blender 4.3 in the background, which is what `tests/run_tests.ps1` uses.
- **`SOURCES.md`** marks the twelve `authored, installed`, keeps the Sketchfab candidates as possible replacements,
  and lists the four left (fds, atari5200, atarijaguar, odyssey2).

## Checks

- **`tests/run_tests.ps1 -Cli <retroverse-cli>`** passes: the normalized messy model and the twelve authored consoles
  all pass `content check` (Blender 4.3, about a minute).
- **Installed into the private real-library data directory** (`retroverse-i11/build/rvdata-real`) with `content
  add-console --replace`: `content check` passes all twelve, each within 10% of its measured size (the ports, vents
  and buttons stand a little proud).
- **The real rooms, by hand** (`--start-room room:<system> --spawn 0.2,0,-3.8,0,-42`, the pose of step 8's close-up
  turned down onto the cabinet): each console stands on the cabinet beside the CRT, in its system's room, 0 refused.
  The run report of `room:nes` counts the three models of its floor (atari7800, mastersystem, nes); `room:psx` its
  one. The screenshots stay under `build/screenshots/step9/`.

## Departures

- **Authored, not downloaded.** The plan wanted the candidates downloaded by the user, normalized or authored. Every
  candidate found is on Sketchfab, which needs the user's login: the twelve were authored from measurements instead,
  as the user asked, and a download can replace any of them later (`normalize_console.py`, then `add-console
  --replace`).
- **Twelve of sixteen.** The Famicom's own measurements were not found (the Disk System drive's are), and atari5200,
  atarijaguar and odyssey2 were not searched: they wait with step 10's batches.
- **The normalizer ignores `facing`.** A downloaded model's slot anchor is still placed on the top; a side- or
  front-loading download would need the kit's socket.
- **The side-facing sockets are checked by their frame, not by eye.** The insertion into the NES's and the
  Intellivision's sockets follows step 7's contract; no GPU test covers a socket that is not on the top.
