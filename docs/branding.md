# The Retroverse logo

Chosen on 2026-11-07 among six directions (the neon wordmark and the R in a picture tube, the others parked). Every file
under `assets/branding/` is written by `scripts/branding.py`, the one source of the geometry: change the script, never
the files.

```powershell
python -I scripts/branding.py                          # rewrite assets/branding
python -I scripts/branding.py --check                  # exit 1 when the files and the script disagree
python -I scripts/branding.py --preview build/branding # also the icon at every size, and the small ones enlarged
```

## What it is

- **The symbol:** a picture tube, a 4:3 screen whose sides bulge outwards, holding an R bent from a neon tube. On its
  64-unit grid the frame is 3.2 units, the R 5.
- **The wordmark:** `retroverse` in lowercase geometric letters, each the centerline of a tube 8 units thick (a fifth of
  the x-height of 40). The o is a circle, the e the same circle opened with a bar at mid height, the r a stem with a
  short arm, the t a stem whose foot turns right, the v two straight strokes, and the s the one elliptical form. The
  letter spacing is optical, set by hand in `WORD`.
- **The lockups:** horizontal (the screen 1.5 times the symbol, centered on the x-height) and stacked (twice the symbol,
  over the wordmark). In a lockup every tube has the wordmark's thickness, as on a real sign, and the frame is two thirds
  of it.
- **Two tubes:** "retro" and the screen are amber, "verse" and the R magenta.

| Use | Amber | Magenta | Ground |
|---|---|---|---|
| Dark grounds (the default) | `#FFC88A` (style.md's tungsten) | `#C766E0` (lighter than the arcade accent `#B34DCE`, for contrast) | charcoal `#171B22` or darker |
| Light grounds (`-light`) | walnut `#493024` | plum `#7E3393` | ivory `#F4EFE6` |
| The icon's glass | | | `#171B22`, inside the frame |

## The files

| File | For |
|---|---|
| `retroverse-symbol.svg` | the symbol alone on a dark ground (UI, documentation) |
| `retroverse-wordmark.svg` | the name alone |
| `retroverse-lockup.svg`, `-lockup-light.svg`, `-lockup-stacked.svg` | splash, title, about screen, README |
| `retroverse-icon.svg` | the icon's large design (40 px and up): the symbol over its glass, readable on light and dark taskbars |
| `retroverse-icon-16.svg` | the icon at 16 px, drawn on its pixel grid |
| `retroverse.ico` | `retroverse.exe`'s icon: 16, 20, 24, 32, 40, 48, 64 and 256 px |

The sizes up to 32 px are not scaled from the large design but drawn again on their own pixel grid (`small_icon()`). The
sides are straight, since the bulge would be under a pixel. The frame is 1 pixel (2 at 32 px) through pixel centers.
The R's strokes are 2 pixels (3 from 24 px) and cover whole pixels, with a row of air between the R and the frame. 256
px is stored as PNG, the others as 32-bit bitmaps. `rv_set_windows_icon()` (`cmake/TargetDefaults.cmake`) embeds the
icon as the executable's first icon resource: Explorer shows it, and SDL registers it as its window class's icon, so
the title bar and the taskbar button show it too.

## Rules

- The symbol below 40 px is the icon (with its glass), never the bare symbol scaled down.
- The wordmark is never set in a font: it exists only as these paths. A size where its tubes would be under 2 pixels
  (the horizontal lockup below about 160 px wide) uses the symbol alone.
- Keep clear space of at least the tube's thickness doubled around a lockup. Do not recolor the tubes outside the table
  above, and do not add console brand colors or shapes (style.md section 17).
- The lobby's neon sign (BACKLOG, "Branding") sweeps a tube along the same centerlines. The script is where they come
  from.
