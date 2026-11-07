# I11 step 2: logos as pictures

ADR-0019 §3, roadmap I11 step 2. A system's logo file (ES-DE's themes, `identity.json`) becomes an RGBA8 sRGB picture
with straight alpha, fitted into a box at its own aspect.

## What was built

- **lunasvg 3.5.0** (with plutovg 1.3.3), from the pinned baseline, private to `rv_assets`' `svg_raster.cpp`.
  `docs/third-party.md` has both.
- **`assets::rasterizeSvg()`** (`rv/assets/svg_raster.hpp`) rasterizes a document held in memory into a box of 1 to
  4,096 pixels a side, at the document's aspect.
  - **Bounds:** at most 1 MiB and 50,000 elements.
  - **Refused before lunasvg sees the document:** a DOCTYPE or entity declaration (entity expansion), a CSS `@import`,
    and an `href` to anything but `#id` or a `data:` URI. This covers `xlink:href` too. The theme logos need none of
    these.
  - **Alpha:** lunasvg's premultiplied ARGB is turned into straight RGBA.
- **`assets::loadLogo()`:** a logo file read with full sharing. An SVG is rasterized at the box's size. A PNG or JPEG
  is decoded upright and resampled down when larger than the box (never up), by **`resampleSrgbAlpha()`**: stb's
  STBIR_RGBA in linear light, so that transparent pixels never bleed into the edges.
- **`assets::fitInto()`:** the size a picture takes in a box.
- **Tests** (`tests/unit/assets/test_svg_raster.cpp`):
  - colours and transparency at known points;
  - an anti-aliased edge;
  - straight alpha;
  - each refused feature, the limits, an in-document `<use>`;
  - PNG logos made smaller or kept;
  - another format refused.

## Departures

- **No logo cache yet.** ADR-0019 §3 caches the rasterized logo, compressed to BC3, under `cache/identity/`. For the
  UI's logos (at most 512 pixels wide, about 45 of them) rasterizing at start costs a few milliseconds each on the
  pool, so step 3 rasterizes them without a cache. The cache and BC3 come with step 4's signs, at up to 2048 pixels,
  where size and memory decide.
- **The GPU proof moves to step 3.** The plan's "logos in a UI probe" needs the textures registered with the UI,
  which is step 3's work in `ProductUi`.
