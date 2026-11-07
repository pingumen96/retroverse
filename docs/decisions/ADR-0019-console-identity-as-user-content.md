# ADR-0019: The consoles' identity as user content: real names, logos, colours and models read from the user's machine

- Status: accepted (the user's approval of the I11 plan on 2026-11-07, after choosing the content's place, the models'
  sourcing and the extras in planning: [i11/plan.md](../i11/plan.md), "Decisions made with the user")
- Date: 2026-11-07
- Amends:
  - style §1's interpretive rule, §7.3 (Tier A), §17.1 and §17.2: an official logo, a real name and a faithful
    console model may be shown when they are the user's content read at runtime (§1 below);
  - specification §19.4 (likewise, nothing of it is packaged);
  - ADR-0010 §2, ADR-0013 §1 and ADR-0014 §5: "signs are generic texts, never a trade name" still binds the shipped
    data (`systems.json`, `building.json`, `platforms/*.json`) and becomes the fallback the scene shows when no identity
    is found (§4);
  - ADR-0001: two dependencies at the same baseline, `lunasvg` (3.5.0) and the existing `stb` port's `stb_truetype` and
    `stb_dxt` headers (§7);
  - ADR-0008 §7: the settings reader's whitelist gains `Theme`; ES-DE's theme folders join what the adapter reads,
    read-only (§2);
  - CLAUDE.md and `docs/reference/safety.md` as recorded under "Rules that change".
- Relates to: ADR-0003 (no scraper, and here no downloader), ADR-0011 (covers as runtime textures, the invented
  fixture names behind goldens), ADR-0013 (the building and its signs), ADR-0014 (the look), ADR-0016 §11 (the
  insertion's socket), ADR-0017 (the panels in VR), ADR-0020 (I11's other half); roadmap I11

## Context

The user asked for the real console names and original logos everywhere, and for real 3D models of the consoles in
their rooms, searched on the internet where needed (2026-11-07). They chose, in planning:
- trademarked content stays local and outside the repository;
- models come from free sources where a good one exists, else they are modelled in Blender from real measurements,
  with the generic prefab as the fallback;
- two extras: a history plaque per room, and the systems' colours.

What exists (the survey of 2026-11-07):
- **Names.** The UI already shows ES-DE's `<fullname>` ("Nintendo SNES (Super Nintendo)") through
  `EsdeAppCatalog::systemFullName`. Style §17.2 allows it: the words describing a system are catalogue data. The 3D
  signs show generic texts from `assets/world/systems.json` ("16-BIT CORNER"), validated as 1 to 32 printable ASCII
  characters. They are part of the plan's `inputDigest`, and `test_world_building.cpp` forbids 42 trade names in
  them. The lettering is a 5x7 uppercase bitmap font.
- **ES-DE's themes.** The install's bundled `linear-es-de` (MIT code) has, keyed by each system's `<theme>`, a
  `system/metadata/<id>.xml` with `systemName` ("Super Nintendo"), `systemManufacturer`, `systemReleaseYear`,
  `systemReleaseDate`, `systemHardwareType`, `systemDescription` (English, with an `it_IT` translation), `systemColor`
  and `systemColorPalette1` to `4`, and a `system/logos/<id>.svg`. All 45 systems of the reference library have both.
  The user's themes folder holds 11 more themes with other layouts and formats (SVG, PNG, WebP). The active theme is
  `coinops-es-de`, with mostly PNG logos.
- **Images.** The decoder takes PNG and JPEG only. `gl::TextureCache::file` takes asset-relative paths; covers reach
  the GPU through the derived-texture path.
- **The console.** One procedural prefab, `console_cartridge` (24 x 7 x 20 cm, sockets `CartridgeSlot` and `Front`),
  is the hero corner's `console` part in every home and handheld room. Arcade, computer and shared rooms have none.
  Theme showcases show a generic `console` or `handheld` item.
  glTF models load through fastgltf, `Anchor_<Id>` nodes become sockets, and a manifest with `source.kind: "user"`
  needs no redistribution approval. Three gaps remain:
  - models are read from the single asset root only;
  - a model that fails to load draws a magenta cube, not its family's fallback;
  - the insertion finds sockets on procedural objects only.
- **Memory.** The busiest real floor held 477.5 MiB of the 512 MiB allowed at Gate M3 and Gate M4.

## Decision

### 1. User content, never shipped

**The system's identity is user content**, like covers and clips. It holds the display name, the maker, the year, the
type, the description, the colours, the logo and the console model.
- **Where it comes from:** read at runtime from the user's machine. Names, logos, descriptions and colours come from
  ES-DE's themes; models come from Retroverse's content folder.
- **Where it never goes:** never copied into `assets/`, fixtures or goldens, and never packaged by I9.
- **Caches:** what is derived from it (rasterized logos, compressed textures) lives under `<user data>/cache/`, like
  covers' derived textures.

**The shipped data stays brand-free**, and the rule for `assets/` stays absolute:
- `systems.json`'s generic signs and the procedural console remain the fallback, and the trade-name test stays.
- Tests and goldens use an invented fixture theme: invented names and original SVG shapes.
- The real identity is checked only through `scripts/real-library.ps1`, its screenshots kept under `build/`.

**There is no downloader** (style §7.2, §17.2):
- The user downloads a model by hand from the page found for them.
- A model enters only through `retroverse-cli content add-console`, which records its provenance.
- Its licence must allow the user's private use, adaptation included. The content is never distributed, so a
  non-commercial licence is acceptable.

**The sources live in a private sibling repository, `retroverse-content`**, never a submodule and never packaged. It
holds the downloads, the Blender files, the authored scripts, the licences and `SOURCES.md`.

### 2. Where the identity is read

**The fields, in order, the first found winning** (each recorded with its source, for `retroverse-cli identity show`
and the run report):
1. `<user data>/config/identity.json`: `theme` (a theme folder's name), `themeRoots` (folders to search before ES-DE's,
   for tests), and per-system overrides of any field;
2. the metadata of `linear-es-de` in the ES-DE install's `themes/`, and its logos;
3. the logos of the active ES-DE theme (`es_settings.xml`'s `Theme`, added to the settings whitelist; the scraper's
   keys stay unread), in the ES-DE home's `themes/` or the install's;
4. ES-DE's `<fullname>` for the name;
5. the generic sign of `systems.json`.

**Theme layouts**:
- metadata: `system/metadata/<theme-id>.xml`, a `<theme><variables>` document with the variables above, the
  description in `<language name="it_IT">` first, then English;
- logos, searched in this order: `system/logos/<id>`, `_inc/systems/logos/<id>`, `<id>/images/logo`, each as `.svg`,
  then `.png`, then `.jpg`.

WebP logos (some of the user's themes) are left out (BACKLOG).

**The key** is the system's `<theme>` from `es_systems.xml`, which defaults to its name.

**The reader is hostile-input safe:**
- folders are found through the discovery's ES-DE home and install, or `identity.json`; a frame-limited run never
  discovers ES-DE by itself;
- files are opened read-only with full sharing;
- size limits: a metadata file at most 256 KiB, an SVG at most 1 MiB with at most 50,000 elements, a PNG at most
  4096 x 4096;
- names go through `rv/core/utf8.hpp`;
- a file that fails is logged once and left out.

**Where it runs:** the identity is built on the catalog worker at the import and the refresh, as `app::SystemIdentity`
per system.

### 3. Logos as textures

- **SVG** is rasterized by lunasvg inside `rv_assets` (`assets::rasterizeSvg`), on the pool: the sign's logo at most
  2048 x 512, the UI's at most 512 wide.
- **Compression:** the result is compressed to BC3 with `stb_dxt` and cached under `cache/identity/`, keyed by the
  file's path, size, time and the requested size.
- **Upload:** the GPU takes it through the derived-texture path, never `TextureCache::file`.
- **The UI** shows it as `rv-texture:logo:<system>`.
- **Memory:** a logo costs at most 1.4 MiB on the GPU, and the signs of a room share one.

### 4. Names and logos where they are shown

**The display name** is the identity's name: the theme's `systemName`, else ES-DE's fullname. It is shown wherever
lists, cards, notices and screens show a system today. ES-DE's fullname, when it differs, is the detail line of the
card and the directory. The directory's room rows and the elevator panel show the logo beside the name, in VR's panels
too.

**The signs.** Wherever a room's sign shows (its neon over the hero corner, the hall's portal, the blade, the hub's
fascia), the logo is shown:
- the neon draws the logo emissive, its alpha the tube, tinted (§5);
- printed panels draw it in its own colours;
- the name stands under it in TrueType lettering (`stb_truetype`, the bundled Atkinson Hyperlegible Bold): mixed case,
  any UTF-8 the font holds ("Odyssey²", "Pokémon Mini").

Fallbacks:
- a system with a name but no logo shows the name alone in that lettering;
- a system with neither shows the plan's generic sign in the bitmap font, as today;
- an expansion keeps its number after the name (`chainSignText`);
- a shared room's portal shows its sections' logos side by side, up to four, then its generic sign.

**The plan never changes.** The identity is applied when the scene is built (`loadSceneData`, `toSceneFile`), outside
the plan, its `inputDigest` and its validation. A world without a theme builds the same scenes as before, and every
committed golden stays 0 pixels apart. One new golden (G22) shows a fixture room with the invented theme's logo, name,
plaque and colour.

### 5. The systems' colours

- **The source:** the identity's palette (`systemColor`, `systemColorPalette1` to `4`).
- **The choice:** the most saturated colour, clamped into style §3's accent band (its saturation and luminance limits,
  so that the 70/20/10 rule holds).
- **Where it goes:** it tints the room's neon and the portal's fascia band, in place of the theme pack's accent.
  Plastics and materials stay the pack's.
- **The setting:** "Colori dei sistemi" (`world.systemColors`, on by default, never a plan key: ADR-0018 §2). Off gives
  today's look exactly.

### 6. The history plaque

- **What it shows:** `room_plaque.rml` (the logo, the name, the maker, the year, the type and the Italian description),
  drawn into a texture through the UI's `renderTo` (as VR's panels are).
- **Where it stands:** a brass plaque on its own small stand beside the hero corner, from the template's free cell
  beside the cabinet. It is placed at scene build, so the plan is unchanged.
- **Interaction:** Focus and Inspect read it as they read a game's card, on the desktop and in VR.
- **Without an identity:** a room without one has no plaque.

### 7. Console models

**The content root.** `Paths::contentDir()` is `<user data>/content`, a second root for scene models, read-only to
everything but `retroverse-cli content`. Scene descriptions address it as `content:<relative path>`; the same rules as
asset paths apply (no `..`, no absolute path).

**A console** is `content/consoles/<system>/`, holding `console.glb` (or `.gltf` with its files) and `console.json`
(schema 1):
- `model`;
- `kind`: `home`, `handheld` or `computer`;
- `front`: the axis, +Z by default;
- `sockets`: names mapping to nodes, else the glTF's `Anchor_CartridgeSlot`, `Anchor_DiscTray`, `Anchor_Front`;
- `scaleCheck`: the expected size in cm;
- `source`: URL, author, licence, date, modifications.

`console.json` is read as a `source.kind: "user"` prefab manifest.

**Validation** (`retroverse-cli content check`, and at load):
- each axis between 5 and 45 cm, and the footprint within the hero corner's free area beside the CRT (about 42 x 40 cm);
- at most 60,000 triangles;
- at most 2 MiB of GPU texture memory after compression: base colour BC1 or BC3, normal BC5, occlusion-roughness-metal
  BC1, through `stb_dxt` on the pool, cached under `cache/content/`;
- only the glTF features the loader takes.

A model that fails any of these is not shown. Its system keeps the procedural console, logged once.

**Where it stands:**
- **Home and handheld rooms:** the hero corner's console part is drawn from the model at scene build, under the same
  fixture id, at its real size on the cabinet, with its collider from its bounds. A handheld stands on a small
  generated stand.
- **Showcases:** a theme showcase with `"item": "console"` or `"handheld"` shows the room's model, scaled into the
  showcase.
- **Computer rooms** (no console part today): the computer stands at the template's console position when it fits,
  else on the room's desk; I11 step 10 decides after measuring.
- **Shared rooms:** each section's model stands in that section's showcases.

**The insertion** (ADR-0016 §11) takes the media socket from the model: `CartridgeSlot`, else `DiscTray`, else the top
centre. `consoleSocketOf()` reads model sockets as it reads procedural ones.

**The fallback for every model, shipped or user,** is its family's procedural prefab, never the magenta cube, which
stays only for a scene's own test models.

**The pipeline** (in `retroverse-content`, not the product):
- `normalize_console.py` runs Blender 4.3 in the background. It imports glb, gltf, fbx, obj or blend, scales to the
  real measurements of `dimensions.json`, puts the origin at the bottom centre with the front +Z, and decimates to the
  budget. It bakes one 1024² base colour and 512² normal and occlusion-roughness-metal maps, adds the anchors and
  exports glb.
- Consoles without a good free model are authored by `author/<system>.py` from measurements and reference photos.
- `retroverse-cli content add-console --system <id> --model <file> --source <url> --author <name> --license <id>`
  copies the result into the content folder and writes its `console.json`. `content list` and `content check` report.
- `retroverse-asset-preview` opens a content folder.

### 8. Tests

- **Unit tests:**
  - the reader on a fixture theme (`tests/fixtures/esde-theme/`, invented systems, original SVG shapes): every field
    and its source, the order, hostile XML and SVG, the limits;
  - the rasterizer against reference PNGs within the goldens' tolerance;
  - the sign's layout per case;
  - the colour clamp's table;
  - the content manifest's rules;
  - the socket derivation.
- **GPU tests:**
  - the fixture identity in the directory, the card, the room's sign and the plaque;
  - an original fixture console (made at test time by `scripts/make_test_models.py`) in the fixture room, with the
    insertion into its socket;
  - G22;
  - every other golden 0 pixels apart.
- **Real library** (`real-library.ps1`, ES-DE closed, read-only snapshots, 0 differences):
  - `identity show` for every system;
  - `content check` on the user's content folder;
  - the real building's memory row.

### 9. Acceptable performance (added to the roadmap's table)

| Measure | Acceptable |
|---|---|
| The busiest floor with the identity's logos and a console model per room (1080p, both screens playing) | all textures at most 512 MiB (the existing row, now with them) |
| The identity read and its logos rasterized for the reference library (45 systems) | within the start's 5 s (NFR-01), off the main thread; warm from the cache |

If the busiest floor passes 512 MiB, the full model textures stay resident only in the walker's room and its
neighbours, and the other rooms show their smallest mip. That is step 10's remedy, not a relaxed limit.

## Rules that change

- **`assets/` stays brand-free**, and the fixtures and goldens with it. A real name, logo or console model is user
  content: read at runtime from ES-DE's themes or `<user data>/content`, never copied into the repository, never
  packaged.
- **Signs may show a logo**, when the logo comes from the user's machine.
- **Retroverse reads ES-DE's themes**, read-only, and `Theme` among the settings' keys.
- **The content folder is written only by `retroverse-cli content`.**

## Consequences

**Gains**
- The building names and shows each console as the user knows it: logos on the signs, the real hardware on the
  cabinet, its story on a plaque.
- Nothing of it enters the product's package, so I9 stays distributable.
- The plan, its determinism and every committed golden are untouched.

**Costs**
- A new dependency (lunasvg) for a hostile format, kept inside `rv_assets` with its bounds.
- A second root for scene models.
- A model pipeline that lives outside the repository and needs Blender.
- The models are manual work per console, and the memory margin of the busiest floor is thin.

**Deferred**
- WebP logos.
- Logos on the generated cartridge labels.
- The real controllers beside the console.
- The insertion in VR.

See `BACKLOG.md`, "Parked at I11's planning".
