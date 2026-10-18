# I3 step 1c: the fixtures of the generated room

Step 1c of I3 (docs/roadmap.md, "I3: M2 SNES vertical slice") gives the three fixture folders step 0a declared their
content: payload generators for `files.json` (covers and other pictures, clips, a whole generated collection), the
fixtures `esde/snes-room` (G02, G10, the damaged media of steps 11, 12 and 14b), `esde/snes-single` (G06) and
`esde/snes-stress` (the budgets, G02s, the benchmark path), each with a README stating what it exercises and the counts
later steps assert, and the tests that materialize them and check those counts with the ROM scan, the library snapshot
and the media resolver. Nothing outside `tests/support`, the three fixture folders and their tests changed.

## What was built

### Payload generators (`tests/support/fixture_payloads.hpp`)

A file of `files.json` holds exactly one of `size` and `seed`, `copyOf`, and now `cover` or `video`:

- **`cover`**: `{"title", "seed", "width", "height", "format", "shape", "damage", "quality"}`, title and seed required.
  The picture is `assets::makeCoverPlaceholder` (abstract geometry from the style §3 palette and the title in the
  built-in bitmap font: original, no logo, no commercial layout), opaque RGB, 680 x 497 by default (the landscape box
  covers scrapers deliver), any side 1 to 4,096. PNG through `assets::encodePng` or JPEG through `assets::encodeJpeg`
  (quality 88), after the file's extension or `"format"`. `"shape": "cartridge"` draws ES-DE's `physicalmedia`: a grey
  shell with rounded corners, grip band and ridges, the cover as its label, on a transparent background (RGBA, PNG
  only). `"damage": "truncated"` keeps the first half of the encoded file (`decodeImage`: ParseError for a PNG);
  `"not-an-image"` writes a line of text under the picture's name (`decodeImage`: Unsupported).
- **`video`**: `{"seed", "seconds", "width", "height", "frameRate", "codec", "quantizer", "damage"}`, seed required: a
  4:3 clip of `media::writeTestVideo` (640 x 480, 30 fps, 4 s, MPEG-4 Part 2 at quantizer 5, a key frame every second;
  the frame number burned in, read back by `media::decodeFrameCounter`) in the container the extension names (`.mp4`,
  `.mkv`, `.avi`; Motion JPEG in the last two). `"damage"`: `empty` (0 bytes: `InvalidFile`), `truncated` (the first
  half: `Truncated`), `corrupt` (`media::writeCorruptedCopy`: `CorruptData`), `unsupported-codec` (an `.avi` whose codec
  tags read "RVZZ": `UnsupportedCodec`), each through a clean clip written beside the target and removed.
- **`encodeQuickPng`**: a PNG writer of its own for the generated collection's 2,300 covers (below).

### Generated collections (`tests/support/fixture_collection.hpp`)

`files.json`'s new top-level `"collections"` array: `{"seed", "titles", "duplicates", "videos", "romFolder",
"romExtension", "romSize", "gamelist", "mediaFolder"}` generates one system's ROM files, gamelist, covers and clips, for
a fixture too large to list file by file. The titles come from word lists of ordinary words in five forms (one with a
`": "` subtitle, sequel numerals, long titles), drawn with `core::Random` from the seed, each title key once; the
regions, second releases (a "(Europe)" beside a "(USA)", or a "(USA) (Rev 1)" beside the plain one), favorites, play
counts, cover formats and orientations follow fixed rules of the title's index, which the header and
`snes-stress/README.md` state. `generateCollection()` is pure and deterministic.

### The materializer (`tests/support/fixture.hpp`)

- Every file of `files.json` and every collection is described and checked first; nothing is written before the whole
  manifest is known to be valid. The files are then written on up to 8 threads (pictures and clips take a while), the
  first failure in the manifest's order reported; times and the hidden attribute are set afterwards, in order.
- The second release of a collection's title shares its picture: encoded once, written to both files.
- `Fixture::applyOverlay(folder)` moves every file of a materialized folder to the same path below the root, replacing
  what is there, and removes the folder: `snes-room`'s `additions/` (G10's ten games added after a first plan).

### The fixtures

| Fixture | What | Counts its tests assert |
|---|---|---|
| `esde/snes-room` | 50 placed titles with covers (one portrait, one square, one large, 8 JPEG, one truncated, one not an image), 10 cartridge pictures, 14 clips (10 that play, in `.mp4`, `.mkv` and `.avi`; corrupt, truncated, empty, unsupported codec; and the missing clip, a game with a cover and no video), two titles released in USA and Europe, a game with a screenshot and a clip but no cover, two hacks without media, one game with every image kind; `additions/` with G10's ten | 55 games, 53 gamelist entries, 52 with a cover, 50 titles, 6 favorites, 26 with a play count; with the additions 65, 63, 62, 60 |
| `esde/snes-single` | one ROM with a gamelist entry; the media root there and empty | 1 game, 0 media |
| `esde/snes-stress` | one generated collection, seed 2000: 2,000 titles, 300 second releases, 4 clips | 2,300 games, all in the gamelist; 2,300 covers (200 JPEG, 60 portrait), 2,000 titles, 80 favorites, 100 revisions, 300 files named "(Europe)" |

The system of all three is ES-DE's `snes` (the name `assets/world/platforms/snes.json` and the steps' commands use) with
the invented full name **Lumen 16 Home System**; one command, "Fake Emulator", finds a copy of `rv_fake_emulator.exe`
under `%ESPATH%`. Settings name the ROM and media folders under the fixture root and hold no scraper credentials. The
READMEs list every file of `snes-room` with its role, priority data and media, and the rules of `snes-stress`.

## Measured

Informative: the machine was fully loaded by the other steps of the wave (CPU at 94 to 100% while measuring), so these
are upper bounds; `esde/bench: snes-stress is materialized within 10 s` (`[.bench]`, Release) is the roadmap's check,
to run on a quiet machine:

| | Release | Debug |
|---|---|---|
| A 680 x 497 cover: the pattern / `encodePng` / `encodeQuickPng` / `encodeJpeg` (one thread, quiet machine) | 3.3 / 23.5 / about 3 / about 5 ms | 31 / 310 / 53 / 44 ms (loaded) |
| `snes-stress` materialized (8 threads; `[.bench]`) | 8.6 s wall, 23.8 s of CPU (CPU at 97%) | 54 s (loaded) |
| `snes-stress` test (materialization, snapshot, media, covers decoded) | 26 s (loaded) | 61 to 70 s (loaded) |
| `snes-room` test (materialization, snapshot, media, every cover decoded, 14 clips played) | 7 to 19 s | 14 to 20 s |

## Tests

- `tests/unit/support/test_fixture_payloads.cpp` (`[support][fixture]`): covers decode as encoded (sizes, channels,
  PNG lossless, deterministic, another seed or title another picture); the quick PNG writer gives back every pixel for
  1 to 4 channels, runs, noise and repeated rows, and a cover through it decodes as through stb_image_write; a cartridge
  is transparent around its shell; damaged covers fail as asked; the `cover` and `video` objects and their refusals
  (unknown keys, ranges, a JPEG cartridge, Motion JPEG in `.mp4`, an unsupported-codec clip that is no `.avi`); a clip
  plays with its frame counter (0 to 9) and leaves nothing beside it; each damaged clip ends with its
  `media::DecodeFailure`; a generated collection follows its rules (2,300 files, 2,000 title keys, 200 + 100 second
  releases, 80 favorites, 200 JPEG, 40 portrait titles, deterministic, another seed other titles); the collection
  object's refusals; a fixture written by the test with covers, a clip, a collection and an overlay; a file of two
  kinds and a collection colliding with a listed file are refused.
- `tests/unit/esde/test_snes_fixtures.cpp` (`[esde][fixture][snes]`): each fixture materialized and read by
  `EsdeLibrarySource` (state counts, gamelist entries, the system's invented full name), its media found by
  `EsdeMediaResolver` per kind, the games with a cover and their title keys counted as room selection will count them,
  favorites and play counts; in `snes-room` every cover decoded (all but the truncated one, ParseError, and the text,
  Unsupported), the cartridge picture with alpha, the truncated screenshot, every clip played to its end or its failure
  (10 play, 4:3; Lilac Loom `CorruptData`, Mossy Meadow Mystery `Truncated`, Nutmeg Nook `InvalidFile`, Opal Oriel
  `UnsupportedCodec`); G10's overlay (the additions' gamelist extends the first, 65 games, 60 titles); `snes-single`'s
  empty media root; `snes-stress`'s counts and its covers decoded (every one in Release, every JPEG and one PNG in ten
  in Debug).
- `esde/bench: snes-stress is materialized within 10 s` (`[.bench]`): wall time and the process's CPU time.

## Decisions taken within the step

1. **The system keeps ES-DE's name `snes`**, only its full name is invented: the room's platform data
   (`assets/world/platforms/snes.json`), the template and every step's commands (`worldplan generate --system snes`)
   name it, and no image shows the short name; the full name is what a generated label may print.
2. **G10's ten games live in `additions/`**, outside the ROM and media roots, with a gamelist that extends the first;
   `Fixture::applyOverlay("additions")` lays them over the root after a first plan. A gamelist naming them from the start
   would have made them missing files in G02's catalog. A test keeps the two gamelists alike in their first 53 entries.
3. **Room selection's expectations are stated, not computed by step 2's code**: the tests count games with a cover and
   their title keys (the title lower-cased, the tags left out) as ADR-0010 §3 defines them, so that step 2 finds the
   same figures (50 titles, 2 `duplicate-title`, 3 `no-cover` in `snes-room`; 2,000 and 300 in `snes-stress`).
4. **Damaged covers are among the 50 placed titles** (Bellows Brigade truncated, Parasol Patrol not an image): a
   present but unreadable cover still makes a game eligible and its slot shows the generated label (steps 5 and 12);
   G02 will show those two labels. The roadmap's "a game without a cover" is Thistle Trellis, which has other media.
5. **A PNG writer of the tests' own for the collection.** stb_image_write takes about 25 ms per 680 x 497 cover in
   Release (its deflate searches hash chains), which put `snes-stress` at 15 s on 8 threads; `encodeQuickPng` (Sub or
   Up filter per row, one fixed-Huffman deflate block whose matches repeat the previous byte) takes a few ms and gives
   a valid PNG of a similar size on these flat pictures. `snes-room`'s covers keep `assets::encodePng`, so both
   encoders' files are among the fixtures. `rv_assets` was not changed (step 5 works in it).
6. **The 10 s bound is a hidden `[.bench]` case**, as I2 step 9 measured its timings: a wall-clock check in the unit
   suite would fail on a loaded machine. The unit test checks the counts only.
7. **ROM payloads are small** (64 to 512 KiB in `snes-room`, 4 KiB in `snes-stress`): nothing reads more than the quick
   hash's two 64 KiB chunks, and 2,300 files of real size would cost seconds of writing.
8. **Clips are short** (4 s, 640 x 480 or 320 x 240; one of 6 s): enough for step 11's frame rate, loop and freeze
   checks, at about 0.6 MB each.

## Open issues

- **The 10 s bound was measured on a loaded machine only**: 8.6 s with the CPU at 97% (the other worktrees of wave 1
  building and testing); before the quick PNG writer the same run took 15 s. The 23.8 s of CPU time on 8 hardware
  threads suggest 3 to 4 s quiet. Run `build/msvc-x64-release/bin/rv_tests.exe "esde/bench: snes-stress*"` on a quiet
  machine (the gate does) and record the figure here.
- **Debug cost**: the `snes-stress` test takes about a minute in Debug on a loaded machine (the pattern itself costs
  31 ms per cover unoptimized, in `rv_assets`). Steps that materialize `snes-stress` in Debug smoke tests should do it
  once per run.
- `unit.app/catalog_worker: a session write that meets a busy database stays pending on the worker, then is written`
  failed once in the Debug suite on the loaded machine and passed when run again alone; nothing of this step is
  involved (a timing of the catalog worker under load).

## Notes for later steps

- **Step 2** (room selection): `snes-room` gives 52 eligible games, 50 representatives; Cobalt Compass (Europe) and
  Gilded Gazebo (Europe) are `duplicate-title` of their USA releases; Thistle Trellis and the two hacks are `no-cover`;
  Opal Oriel (Europe), Woolly Abacus (Japan) and Hazel Hedgerow (USA) (Rev 1) are their titles' only releases.
  `snes-stress`: 2,000 placed, 300 `duplicate-title`, where for the 100 revision pairs the "(USA) (Rev 1)" file is the
  representative and the plain "(USA)" the duplicate.
- **Steps 3 and 9**: `snes-room`'s six favorites and 26 play counts order the priority; G10 is
  `fixture.applyOverlay("additions")` after the first plan, then a refresh. `snes-stress` has 80 favorites.
- **Step 5**: the room's covers include a portrait (Umber Umbrella, 497 x 680), a square (Velvet Vineyard, 512 x 512),
  a large one (Willow Windmill, 1,360 x 994), JPEGs, a truncated PNG and a text named `.png`; the cartridge pictures
  are RGBA. `rv::test::PicturePayload` and `pictureBytes()` make any other picture a test needs (16 px, 4,096 px).
- **Step 11**: the clips and their failures are in `snes-room/README.md`; "the missing clip" is Pewter Parasol (a cover,
  no video); a clip that the catalog recorded and that is gone is made by deleting one after the import. Every valid
  clip's frames carry their number (`media::decodeFrameCounter`).
- **Step 12**: renaming `media/` after a first run is the "media root unavailable" case; covers are written at
  materialization time (their last write time is the materialization's).
- **Step 14b**: Amber Lantern has every image kind but manuals and custom; Bramble Burrow Rally's screenshot is
  truncated (the corrupt image's fallback); Thistle Trellis has a screenshot but no cover.
- **Step 17**: no golden shows a real title or name: titles and companies are invented, the system's full name is
  "Lumen 16 Home System". Debug materializes `snes-stress` in about a minute (loaded machine).

## For the merge

- **CLAUDE.md** (not edited by the step): "Layout and layering", the `tests/support` sentence on `Fixture`:
  files.json's `cover` and `video` payloads (`fixture_payloads.hpp`), `"collections"` (`fixture_collection.hpp`),
  `Fixture::applyOverlay`; the fixture list: `esde/snes-room`, `esde/snes-single`, `esde/snes-stress`, their READMEs
  stating the counts.
- **docs/roadmap.md**: step 1c done; the 10 s figure from the quiet-machine run of the `[.bench]` case.
