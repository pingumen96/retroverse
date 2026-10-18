# I3 step 5: the cover and label pipeline

Step 5 of I3 (docs/roadmap.md, "I3: M2 SNES vertical slice", row 5; wave 1, beside 1c, 2, 3, 4, 6, 7 and 13r) fills the
seven `rv_assets` stubs step 0a pre-listed for it (`cover_fit`, `mip_chain`, `bc1`, `text_raster`,
`collection_labels`, `rvtex`, `derived_texture_cache`) with the GPU-free half of
[ADR-0011](../decisions/ADR-0011-collection-rendering-and-cover-textures.md): what a cartridge's front and spine show
(§5), how a layer is made on a worker (§6: decode, fit, mips in linear light, BC1, the dominant colour, text), and the
derived-texture cache (§7). Nothing calls it yet: step 6 uploads the layers into the texture arrays, step 12 is the
loader that runs this on the worker pool, serves the cache and swaps the layers in. No CMake list changed (0a listed
every source), no GL, no new dependency (`stb_image_resize2.h`, `stb_dxt.h` and `stb_truetype.h` come with the pinned
`stb` port).

## What was built

| File (`src/assets`) | What it does |
|---|---|
| `mip_chain.hpp/.cpp` | `Rgb8`, `LinearRgb`; exact sRGB codes to and from linear light (`srgbByteToLinear`, `linearToSrgbByte`: a table and the 255 thresholds between codes, so every code round-trips); `mipLevelCount`, `mipLevelSide` (OpenGL's sizes); `buildMipChain` and `reduceLinear`, an exact box filter on linear values |
| `bc1.hpp/.cpp` | `encodeBc1` (stb_dxt, high quality, edge pixels repeated into whole blocks), `decodeBc1` (the reference decoder: S3TC's four- and three-colour blocks, stb_dxt's interpolation), `bc1LevelBytes`, `psnrRgb` |
| `rvtex.hpp/.cpp` | `TextureClass` (front, spine), `TextureFormat` (BC1 sRGB, RGBA8 sRGB), `layerSize`, `layerBytes`, `RvTexture` (a layer with its chain, dominant colour, source stamp), `makeLayerTexture`, `decodeLevel`, the `.rvtex` file (`serializeRvtex`, `parseRvtex`) |
| `cover_fit.hpp/.cpp` | `fitCover` (ADR-0011 §5's rules, stb_image_resize2), `dominantColour`, `buildCoverLayer` (the cold path of one cover: decode, fit, dominant colour, mips, encoding) |
| `text_raster.hpp/.cpp` | `FontFace` (stb_truetype; `loadBundled` reads the font's entry in `<asset root>/registry/assets.json` and refuses a file with another SHA-256 before stb_truetype sees it), `decodeUtf8`/`encodeUtf8`, `GlyphRun`, `drawText` (twice the size, reduced, mixed in linear light) |
| `collection_labels.hpp/.cpp` | contrast (`relativeLuminance`, `contrastRatio`, `labelColours`), the label palette (`kDefaultLabelPalette`, `paletteColour`), the title's choice and layouts (`chooseTitleText`, `layoutSpineTitle`, `layoutFrontTitle`), `drawSpine`/`buildSpineLayer`, `drawFrontLabel`/`buildFrontLabelLayer`, and `buildFrontLayer` (the cover, or the generated label when there is none or it fails) |
| `derived_texture_cache.hpp/.cpp` | `textureCacheRoot`, `CacheKey` (`coverCacheKey`, `generatedCacheKey`, `spineCacheKey`, `frontLabelCacheKey`), `DerivedTextureCache` (`lookup`, `store`, `markServed`, `trim`, `stats`), `serveCover`, `serveSpine`, `serveFrontLabel` |

Each stb implementation is compiled in exactly one source with internal linkage (`STB_IMAGE_RESIZE_STATIC`,
`STB_DXT_STATIC`, `STBTT_STATIC`), like `stb_image` already was, so Dear ImGui's own copy of stb_truetype never clashes.
None of them keeps global state (stb_dxt since 1.11), so every function may run on several workers at once; a
`FontFace` is immutable once loaded and shared by the workers; `DerivedTextureCache` counts with atomics and guards its
"served in this run" set with a mutex.

Typical use on a worker (step 12 will do this, with the paths derived on the catalog worker):

```cpp
auto title = rv::assets::FontFace::loadBundled(paths.assetRoot(), rv::assets::kLabelTitleFontPath);   // once
auto detail = rv::assets::FontFace::loadBundled(paths.assetRoot(), rv::assets::kLabelDetailFontPath);
const rv::assets::LabelFonts fonts{&*title, &*detail};
rv::assets::DerivedTextureCache cache(rv::assets::textureCacheRoot(paths.cacheDir(), libraryId).value());
const auto trimmed = cache.trim(rv::assets::kDefaultTextureCacheBytes, entriesOfTheShownPlan);     // at start

auto front = rv::assets::serveCover(cache, format, "covers", "snes/covers/Game (USA).png", mediaRoot / ...);
auto spine = rv::assets::serveSpine(cache, fonts, {title, fileNameTitle, front ? std::optional(front->texture.dominant)
                                                                                : std::nullopt, seed}, format);
if (!front) { /* no cover, unreadable, not an image: */ rv::assets::serveFrontLabel(cache, fonts, label, format); }
```

## Formats

| Class | BC1 sRGB (`GL_COMPRESSED_SRGB_S3TC_DXT1_EXT`) | RGBA8 sRGB fallback | Levels |
|---|---|---|---|
| Front | 256 x 188, 32,280 bytes with its mips (24,064 for level 0) | 128 x 94, 64,012 bytes | 9 (BC1), 8 (RGBA8), down to 1 x 1 |
| Spine | 40 x 256, 6,984 bytes | 20 x 128, 13,628 bytes | 9 (BC1), 8 (RGBA8) |

39,264 bytes per item in BC1 (ADR-0011 §9's 39 KiB), 77,640 in RGBA8 (1.98 times). Level sides halve rounding down
(OpenGL's rule: 188, 94, 47, 23, 11, 5, 2, 1, 1); a BC1 level is whole 4 x 4 blocks in row-major order, the blocks past
the right or bottom edge holding the last column or row repeated, so that the GPU's sampling at the edge never mixes in
padding; step 6 uploads each level whole with `glCompressedTextureSubImage3D`. An RGBA8 level is width x height x 4
bytes, rows top to bottom (ADR-0002: images never flipped).

The `.rvtex` file (`rvtex.hpp` has the table): a 96-byte header, little-endian (magic `89 52 56 54 58 0D 0A 1A`, file
version 1, pipeline version 1, class, format, mip count, flags, width, height, dominant colour, payload bytes, the
source's size and FILETIME, the cache key's SHA-256, FNV-1a 64 of the payload and of the header), then the levels,
largest first. Every field is checked on reading; a front entry is 32,376 bytes on disk, a spine 7,080.

## What a face shows

- **Front, a cover**: decoded with `assets::decodeImage` (PNG or JPEG only; palettes, grey and 16-bit expanded to RGBA),
  alpha composited over a matte in linear light (covers are opaque; a dark neutral grey, or the family's plastic colour
  the caller passes), then fitted to the class's size: scaled to cover and cropped at the centre when that cuts at most
  8% of the cropped side, else scaled to fit with bands of the mean colour (in linear light) of the two edge strips
  facing them, each 2% of the side. stb_image_resize2 resamples the sRGB data in linear light (Mitchell reducing,
  Catmull-Rom enlarging, clamped edges), with a fractional input rectangle, so the crop is exact. The survey's 680 x 497
  covers lose 0.47% of their width; the 476 x 864 portrait ones get a 104 px wide cover between 76 px bands.
- **The dominant colour**: of the cover's part of the fitted picture (bands excluded), the fullest of 4,096 bins (4 bits
  per channel; the lowest on a tie), the mean of its pixels in linear light. Kept in the layer (`RvTexture::dominant`)
  and in its cache entry, so a spine can be drawn while the media root is unplugged.
- **Spine**: always generated. The title (white space collapsed; a title with no character the font has, a Japanese one
  in the Latin font, uses the catalog's cleaned file-name title; a character the font lacks is drawn as its .notdef,
  since Atkinson Hyperlegible has no U+FFFD) on the cover's dominant colour, or the palette's colour for the
  placement's seed without a cover. The text is ivory `#F4EFE6` or charcoal `#171B22` (style §3), whichever contrasts
  more; the background is mixed towards black (ivory text) or white in linear light in 2% steps until the contrast
  reaches 4.8:1, a margin over WCAG's 4.5:1 for BC1's 5:6:5 endpoints. The title reads top to bottom: set horizontally
  and turned 90 degrees clockwise, the line starting at the layer's top row, the glyphs' tops towards +x. Sizes are
  shares of the thickness T (40 px): one line from 0.62 T down to 0.45 T; else two lines from 0.45 T down to 0.34 T,
  split at a separator (`" : "`, `": "`, `" - "`; the colon stays on the first line, the dash goes) when one lets them
  fit, else between the words that let the larger size fit; else two lines at 0.34 T, the separator's first part or as
  many words as fit on the first, the rest cut with an ellipsis (U+2026); a single word too long stays on one line at
  0.45 T, cut. 13 px margins at each end (5%), lines centred both ways, baselines 0.92 sizes apart.
- **Generated front label** (no cover, or one that fails): the palette's colour for the seed (style §3's bordeaux,
  electric blue, sign green, arcade magenta, oak, graphite, tungsten amber, ivory), adjusted like a spine's; a 3 px
  frame inset 7 px and two 4 px stripes across the top, in tints of the text colour; the title (bold) centred between
  them: one line from 34 to 20 px, else two or three from 26 to 14 px (a separator forces the first break, each part
  broken where its widest line is narrowest, the largest size winning unless fewer lines reach 85% of it), else three
  lines at 14 px with the last one cut; the system's name (regular, cut to the room the year leaves) at the bottom left
  and the year at the bottom right, 14 px. Every size scales with the layer (half in RGBA8).
- **Text**: stb_truetype from `ui/fonts/AtkinsonHyperlegible-Bold.ttf` (titles) and `-Regular.ttf` (details), each
  loaded only when its SHA-256 equals its entry in `assets/registry/assets.json` (the font's registry notes now name
  this use; `docs/third-party.md` names the three stb headers). Glyphs are laid out with the font's advances and kerning
  (no hinting, so widths scale exactly with the size and one measurement sizes a line), drawn as coverage at twice the
  layer's size with subpixel positions (the larger coverage where kerned glyphs overlap), reduced by a 2 x 2 box, and
  mixed with the background in linear light.
- **Mips**: every level from the previous one by an exact box filter on linear values (each source pixel weighted by the
  share of it a destination pixel covers, so 47 -> 23 and 5 -> 2 lose nothing), encoded back to sRGB by rounding in the
  encoded domain; alpha filtered as it is.

## The cache

`<user data>/cache/textures/<library id>/<front|spine>/<2 hex>/<sha256>.rvtex` (`textureCacheRoot(paths.cacheDir(),
id)` refuses an id that is not one plain path element). The name is the SHA-256 of a NUL-separated tuple:

- a cover: `rv-tex/1`, the pipeline version, the class, the format, `media`, the media kind, the media file's path
  relative to the media root as the catalog derives it;
- a generated layer: `rv-tex/1`, the pipeline version, the class, the format, `text`, the number of texts, the texts
  (a spine: title, file-name title; a label: title, file-name title, system name, year), the base colour as `#RRGGBB`
  (the cover's dominant colour or the palette's), the fonts' SHA-256.

`serveCover` opens the source for reading with full sharing (`core::SharedFileReader`) and compares its size and
FILETIME (from the open handle) with the entry's: equal, the entry is served without reading the file (a hit); else the
file is read, the layer built, stamped and stored atomically (`core::writeFileAtomic`) and served (`Built` or
`Rebuilt`). When the source cannot be opened or read (NotFound, Unavailable, Conflict, IoError: a media root unplugged,
renamed or unreachable, a cover deleted) a valid entry is served as it is (`CacheHitSourceUnavailable`); without one
the source's error comes back and the caller shows the generated label. A cover that is not a usable image returns its
decoding error and writes nothing. An entry that fails any check (magic, versions, sizes, checksums, a key that is not
its name's) is deleted and rebuilt, or, without its source, deleted and not served. A stale entry stays until its
replacement is written, so a source that changed and then became unreadable still finds the old picture.

Size: `markServed` sets an entry's modification time the first time a run serves it (a lookup that hits, or a store),
once per entry per run; `trim(maxBytes, inUse)` (`kDefaultTextureCacheBytes`, 512 MiB, at start on a worker) removes
the entries with the oldest times first (then by name) until the total is within the bound, never one whose hex name is
in `inUse` (the shown plan's), and also the temporary files of interrupted writes older than an hour; files that are
not entries are left alone. `stats()` counts hits (and those without their source), misses, stale and corrupt entries,
stores and failed stores, for the run report (step 12). The cache only ever writes under its root: the sources are
opened for reading, nothing is created next to them.

## Measured

Release, the reference machine (8 hardware threads), 2026-10-18, taken inside the wave (other builds may have run):
informative, the gate measures on a quiet machine.

`rv_tests.exe "[bench][covers]"` (generated covers, 680 x 497 PNG of 538 KiB, like the survey's):

| Measure | p50 | p95 | max |
|---|---|---|---|
| **One cover to its BC1 layer** (decode, fit, mips, BC1; budget 25 ms median) | **12.8 ms** | 13.4 ms | 13.7 ms |
| decode (stb_image, PNG) | 7.3 ms | 7.6 ms | 7.9 ms |
| fit (stb_image_resize2, sRGB, 680 x 497 to 256 x 188) | 1.6 ms | 1.7 ms | 1.9 ms |
| mip chain in linear light | 2.0 ms | 2.3 ms | 2.5 ms |
| BC1, stb_dxt high quality, 9 levels | 1.6 ms | 1.7 ms | 1.8 ms |
| spine (layout, text at twice the size, mips, BC1) | 1.0 ms | 1.2 ms | 1.3 ms |
| generated front label | 3.0 ms | 3.7 ms | 3.9 ms |
| cache, cold (read, build, store) | 19.8 ms | 24.3 ms | 27.9 ms |
| cache, warm (source checked, entry read) | 0.45 ms | 0.53 ms | 0.61 ms |

The worker pool (5 threads) served 120 covers cold in 720 ms, 167 covers per second: the reference room's 890 covers
would take about 5.4 s cold (budget 10 s) and 2,000 about 12 s (budget 15 s), before uploads; warm, 890 entries are
about 0.4 s of one thread's reads. A store costs about 7 ms of the cold path, most of it `core::writeFileAtomic`'s
flush (see "Notes for later steps").

The real library through `./scripts/real-library.ps1 ... -Filter "[real-library][covers]"` (Release; 0 differences
among 542,120 entries in the snapshots before and after): every 13th of the 1,553 files under the media root's
`snes/covers` (sub-folders included), 120 covers, 58.2 MiB.

| Measure | Result |
|---|---|
| Sizes | 98 at 680 x 497, 5 at 680 x 500, 10 portrait (476 or 478 x 864), 6 large scans (1,521 to 2,067 x 1,062 to 1,440), 1 at 581 x 402 |
| Fit | 110 cropped (all within 8%), 10 banded (the portrait ones) |
| From memory: decode, fit, mips, BC1 | p50 17.3 ms (budget 25 ms), p95 71.8 ms, max 120.8 ms (the 2,050 x 1,440 scans) |
| Cold through the cache (read, build, store) | p50 19.7 ms, p95 87.1 ms, max 310 ms |
| Warm (source checked, entry read) | p50 0.30 ms, p95 0.33 ms, max 34 ms (one disk stall) |
| BC1 against RGBA8, level 0 (256 x 188) | worst 26.1 dB, median 30.0 dB, best 36.3 dB |
| BC1 against RGBA8, level 2 (64 x 47) | worst 22.1 dB, median 25.4 dB, best 32.6 dB |
| Spine text contrast after BC1, the covers' colours | worst 5.16:1 |

The generated covers measure 36.8 dB at worst (12 covers, `test_bc1.cpp`), the roadmap's criterion of at least 32 dB on
the fixtures' covers; the real scans hold much more fine detail (small print, screenshots, halftone) and lose more to
BC1's two colours per block, more so on the reduced levels, where every block averages more detail. That is BC1's
known cost, accepted by ADR-0011 §4 for the memory it saves; whether it shows at shelf distance is for step 12's
screenshots and step 16's review to judge (see "Notes for later steps"). The spines keep their contrast whatever the
cover's colour.

## Tests

`tests/unit/assets` (`[assets]` plus one tag per file; helpers in `cover_test_support.hpp`: generated covers, a palette
PNG writer, the bundled fonts, pixel hashes):

- `test_mip_chain.cpp`: every sRGB code round-trips (and matches `core::srgbToLinear`), 0.5 is 188; level counts and
  sides; a black and white checker reads 188 +- 1 on every level of a 256 x 188 chain, odd sides included; flat colours
  stay exact, a one-pixel edge weighs its share; determinism.
- `test_bc1.cpp`: block counts; the reference decoder against hand-computed four- and three-colour blocks and 5:6:5
  expansion; flat colours within one code, a two-colour block exact; a 5 x 3 level's padding repeats its edge; BC1
  against RGBA8 at least 32 dB on 12 generated covers (landscape and portrait, fitted); determinism.
- `test_rvtex.cpp`: the classes' sizes and bytes (ADR-0011 §9's 32 and 7 KiB); a layer's chain in both formats; a file
  round trip byte for byte (with and without a source stamp); every damage refused (short, truncated, trailing bytes,
  magic, either checksum, another key's file), a broken chain never written.
- `test_cover_fit.cpp`: landscape, portrait, square, 16 px, 4,096 px, a banner and the exact size never stretched (the
  scaled cover keeps the source's aspect within half a pixel, or the kept part has the label's aspect); the 8% bound
  just under and over, for each axis; bands of the edges' mean colour; an indexed PNG with a transparent entry, grey with
  alpha over the matte in linear light, JPEG, the RGBA8 size; the dominant colour; and every failure (a truncated PNG
  and JPEG, not an image, wider than 16,384 px, an empty file) falling back to the generated label with its error.
- `test_text_raster.cpp`: UTF-8 decoding (malformed sequences, overlong forms, surrogates, truncation); the bundled
  fonts through the registry with their SHA-256; a font with one byte changed, the other weight under the bundled name,
  a missing entry or file, a non-relative path and a non-font with a matching hash all refused; missing characters laid
  out as the replacement glyph; widths scale exactly; drawing at twice the size gives solid stems and antialiased edges.
- `test_collection_labels.cpp`: contrast and the colours (every palette colour and a grey ramp reach 4.8:1; the palette
  covered by 64 seeds); the spine's lines, splits and ellipses as a table (one line, separators of the three kinds,
  words, a long title and an unbreakable word cut, white space), each line within the length less its margins and at a
  size within the rules; accented Latin, a Japanese-only title (the file-name title used), a mixed one (replacement
  glyphs), nothing drawable; the front label's lines as a table; the spine's text at least 4.5:1 after BC1 and in
  RGBA8 on 21 backgrounds; the spine's orientation; generated pictures and encoded layers hashed (Debug and Release
  equal).
- `test_derived_texture_cache.cpp`: the folder, the keys (every tuple element counts, the elements separated, never the
  source's name in the path); a cover round trip byte for byte, a second run's hit, the RGBA8 entries apart, the media
  root's files unchanged (their bytes and their file records' size and time: a directory listing's copy of those may
  lag behind a file just written, which made a first version of this check flaky); an entry served with the media root renamed, on an unplugged drive letter and with the cover
  deleted (and the source's error without an entry); another picture and another time rebuilt, a broken source's error
  with the stale entry kept; a flipped byte, a truncation and another key's file deleted and rebuilt identically, and
  never served without their source; trimming oldest first to the bound, the entry in use kept, old temporary files
  removed and recent ones and foreign files left; the time renewed once per run; four workers serving 12 covers twice at
  once.
- `test_cover_bench.cpp` (`[.bench][bench][covers]`, Release): the 25 ms budget and the figures above.
- `test_cover_real.cpp` (`[assets][covers][.real-library]`, through `scripts/real-library.ps1`): the real sample.
- `test_label_preview.cpp` (`[.label-preview]`): writes sample spines, labels and fitted covers, as drawn and after
  encoding, to `build/<preset>/label-preview` for a visual review (reviewed for this step: the spine reads top to bottom,
  two lines and the ellipsis, the label's frame, stripes and lines, the portrait cover's bands; BC1 indistinguishable at
  1:1).

## Decisions taken within the step

1. **The format is part of every cache key.** ADR-0011 §7's tuple names the class but not the format; a run with
   `--no-texture-compression` (RGBA8 at half the resolution) must never read a BC1 entry as its own, so the format
   joins the tuple and the two formats keep entries apart.
2. **`": "` is a separator too.** ADR-0011 §5 names `" : "` and `" - "`; English titles write `"Title: Subtitle"`, so a
   colon followed by a space splits as well (the colon stays on the first line, a dash is dropped). The spine tries the
   separators before words; the front label breaks at the first separator.
3. **The text's colour and the contrast target.** Ivory or charcoal (style §3's two text colours), whichever contrasts
   more with the base; the base is darkened or lightened in linear light in 2% steps to 4.8:1, so that the layer keeps
   4.5:1 after BC1 (the tests measure the encoded layers, see "Measured").
4. **Sizes.** The spine's sizes are shares of its thickness (one line 0.62 to 0.45 T, two lines 0.45 to 0.34 T): at 1 m
   a 2.0 cm spine covers about 25 px, so the smallest text is about 8.5 px tall on screen (the size stb_truetype calls
   pixel height, ascent to descent), the roadmap's "spine titles at least 8 px tall" for step 16. The front label's
   sizes are in its own pixels (34 to 20 px on one line, 26 to 14 px on two or three).
5. **Balanced lines and fewer lines on the front label.** Each part is broken where its widest line is narrowest (no
   orphan word on a last line); among two and three lines the largest size wins unless fewer lines reach 85% of it.
   The spine's two lines take the split that lets the larger size fit, which is the same rule for two lines.
6. **Bold titles, regular details.** Both bundled weights are used (each checked against its own registry entry): bold
   reads better at a spine's sizes; the system name and year are secondary.
7. **The replacement glyph is the font's .notdef.** Atkinson Hyperlegible has no U+FFFD, so `FontFace` falls back to
   glyph 0 (its .notdef box), which ADR-0011 §5 calls the font's replacement glyph. It has U+2026, so the ellipsis is
   one glyph (three dots otherwise).
8. **The label palette is in code** (`kDefaultLabelPalette`, style §3's roles): no theme data holds one yet (step 1a's
   preset has none). `paletteColour` takes any palette; when a theme gains one, the label requests can carry it
   (a BACKLOG candidate, below).
9. **Alpha over a matte.** ADR-0011 counts covers opaque; a cover with transparency is composited in linear light over
   `FitOptions::matte` (a dark neutral grey unless the caller passes the family's plastic colour) instead of being
   refused.
10. **The band colour and the dominant colour.** Bands take the mean of the two edge strips facing them (2% of the side,
    at least one pixel) in linear light; the dominant colour is the fullest of 4,096 colour bins of the cover's part of
    the picture (bands excluded), averaged in linear light.
11. **The source's stamp comes from the open handle.** `serveCover` compares `SharedFileReader::facts()` (the file
    record) with the entry's, and stamps a new entry with the facts it read the file under. A directory listing's facts
    can lag behind the file record on NTFS (`core::listedFileFacts`), so the cache never mixes the two.
12. **An entry is served whenever its source cannot be read**, not only when the media root is unavailable: a cover
    deleted or locked keeps its picture until the catalog's next refresh drops it from the plan (the room's media
    paths come from the catalog's stored presence, ADR-0011 §6). A stale entry is never deleted, only replaced once the
    new one is written; a corrupt one is deleted and, without its source, not served.
13. **Checksums and the key in the header.** FNV-1a 64 over the payload and over the header, and the key's SHA-256: a
    torn or foreign file is detected before its levels are used.
14. **Trimming.** By modification time, then name (deterministic among equal times); temporary files of interrupted
    writes older than an hour go too (a younger one may be a write in progress); files that are not entries stay.
15. **stb_dxt's high-quality mode** (two refinement steps): it fits the budget with room (see "Measured").
16. **The reference decoder** follows stb_dxt's interpolation (`(2a + b) / 3`, truncated); a GPU may round the third
    points differently by one code, which the tests' tolerances absorb.

## Notes for later steps

- **Step 6** (texture arrays): `layerSize()` and `layerBytes()` size the arrays; a layer's `levels` are uploaded level
  by level, each whole (`levelBytes()`: whole BC1 blocks), 9 levels in BC1 and 8 in RGBA8; `TextureFormat::Rgba8Srgb`
  is the fallback's format at half the resolution. Layer 0, the pending picture (a neutral label in the family's
  plastic colour), is a flat picture through `makeLayerTexture()`; it is not cached.
- **Step 12** (the loader): load both fonts once (`FontFace::loadBundled` with `kLabelTitleFontPath` and
  `kLabelDetailFontPath`; a refused font means no spines: log it and keep the pending layer), make one
  `DerivedTextureCache` per run and library (`textureCacheRoot(paths.cacheDir(), libraryId)`), `trim()` it at start on
  a worker with the shown plan's entries (`coverCacheKey`, `spineCacheKey`, `frontLabelCacheKey(...).hex()`), then per
  item `serveCover` (on an error `serveFrontLabel`), and `serveSpine` with the cover's dominant colour (also when the
  cover came from the cache without its source). Errors of `buildCoverLayer` carry the user's path as context: log them
  at debug level only. `stats()` feeds the run report's cache hits and misses. Nothing here waits or sleeps; a job's
  cancellation is checked between the calls.
- **Step 1c** (fixtures): the roadmap's "BC1 against RGBA8 at least 32 dB on the fixture's covers" is checked here on
  this step's own generated covers (`cover_test_support.hpp`); once 1c's cover generator is merged, `test_bc1.cpp` can
  take its covers instead.
- **Step 16** (look development): `[label-preview]` writes sample layers for review; the label's sizes and colours are
  constants at the top of `collection_labels.cpp`; any change of pixels raises `kCoverPipelineVersion` (the hashes in
  `test_collection_labels.cpp` fail until it is done, and the cache then names new entries).
- **Real covers and BC1** (an open point for steps 12 and 16): on the real sample BC1 keeps a median 30.0 dB against
  RGBA8 at 256 x 188 and 25.4 dB at 64 x 47 (worst 26.1 and 22.1 dB), against 36.8 dB at worst on generated covers.
  If the shelves' covers look blocky in the room's screenshots, the remedies within ADR-0011 are encoding the small
  levels (or the whole chain) in RGBA8 for the fronts, or BC7 later (I8): measure them on the room, not here.
- **The store's flush**: `core::writeFileAtomic` flushes every entry to the disk before its rename (about 7 ms of a
  19.8 ms cold serve). A derived cache needs atomicity, not durability (a lost entry is rebuilt, a torn one fails its
  checksums), so step 12 may want an atomic write without the flush in `rv_core` if the cold budget gets tight.
- Hero textures (ADR-0011 §8) are step 12's: `fitCover` at a larger size, without BC1, serves them.

## For the merge

- **CLAUDE.md** (not edited by the step), a bullet under "Layout and layering" or "Conventions": the collection's
  layers in `rv_assets` (`cover_fit`, `mip_chain`, `bc1`, `text_raster`, `collection_labels`, `rvtex`,
  `derived_texture_cache`; GPU-free, safe on the worker pool): fronts 256 x 188 and spines 40 x 256 in BC1 sRGB (RGBA8
  at half the resolution as the fallback), mips in linear light, spines and generated labels drawn with stb_truetype
  from the bundled fonts only when their SHA-256 match `assets/registry/assets.json`, the derived-texture cache under
  `<user data>/cache/textures/<library id>` (512 MiB, found without the source, invalidated by size and time); raise
  `kCoverPipelineVersion` with any change of a layer's pixels. Tests: `rv_tests.exe "[label-preview]"` writes sample
  layers to `build/<preset>/label-preview` for a visual review; `[bench][covers]` in Release; the real sample through
  `./scripts/real-library.ps1 -Filter "[real-library][covers]"` (its cache in `build/<preset>/real-library-covers`).
- **docs/architecture.md**: `rv_assets`' line gains "the collection's cover and label pipeline and the derived-texture
  cache (I3 step 5)".
- **docs/roadmap.md**: row 5 done (edited here).
- **BACKLOG.md**: "A label palette per theme": the generated labels' palette lives in code (`kDefaultLabelPalette`)
  until a theme's data holds one (style §14's packages).
- **docs/third-party.md** and **assets/registry/assets.json**: edited here (the stb row names the three headers; both
  fonts' notes name their rasterization).
- Every picture the tests use is generated; nothing is added under `tests/fixtures` or `assets/`.
