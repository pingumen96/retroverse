#!/usr/bin/env python3
"""The Retroverse logo (docs/branding.md): the one source of its geometry. Writes the SVG masters and the executable's
icon under assets/branding.

    python -I scripts/branding.py                   write the files
    python -I scripts/branding.py --check           exit 1 unless the committed files equal what this script writes
    python -I scripts/branding.py --preview <dir>   also write the icon at every size, and enlarged, as PNG into <dir>

The symbol is a picture tube (a 4:3 screen with bulging sides) holding an R bent from a neon tube. The wordmark is
"retroverse" in lowercase geometric letters, each drawn as the centerline of a tube 8 units thick, a fifth of the
x-height: the o a circle, the e the same circle opened with a bar at mid height, the r a stem with a short arm, the s
the one elliptical form. "retro" and the screen are the amber tube, "verse" and the R the magenta one. In the lockups
every tube of the symbol is as thick as the wordmark's, as on a real sign; the lobby's neon sign (BACKLOG) will sweep
a tube along the same centerlines.

The icon (retroverse.ico, 16 to 256 px) is rasterized here with the standard library only: a stroke covers a pixel by
the distance of its center to the flattened centerline, the glass by its signed distance to the outline, composited
in sRGB as a browser does. The sizes up to 32 px are drawn again on their own pixel grid (small_icon: straight
sides, strokes of whole pixels), the larger ones use the large design; 256 px is stored as PNG, the others as 32-bit
bitmaps.
"""

import argparse
import math
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "branding"

# Colors (sRGB): style.md section 3, the tungsten amber; a magenta lighter than the arcade accent #B34DCE, which keeps
# its contrast on charcoal; the charcoal of the UI panels as the icon's glass; walnut and a dark plum on light grounds.
AMBER = "#FFC88A"
MAGENTA = "#C766E0"
GLASS = "#171B22"
WALNUT = "#493024"
PLUM = "#7E3393"

# The symbol, on a 64-unit grid. The screen: a 4:3 tube face whose sides bulge outwards.
SCREEN = ("M10 12 Q32 9.6 54 12 Q58.2 12.4 58.6 16.6 Q60.2 32 58.6 47.4 Q58.2 51.6 54 52 "
          "Q32 54.4 10 52 Q5.8 51.6 5.4 47.4 Q3.8 32 5.4 16.6 Q5.8 12.4 10 12 Z")
# The R: stem, bowl, leg.
R_LARGE = "M24 44 V20 H34 A6.5 6.5 0 0 1 34 33 H24 M31.5 33 L40 44"

# The wordmark's letters as tube centerlines: baseline y 60, x-height y 20, the t's ascender y 6; x from each letter's
# own origin, with its width.
TUBE = 8.0
GLYPHS = {
    "r": ("M0 60 V20 M0 40 A20 20 0 0 1 20 20 H22", 22),
    "e": ("M0 40 H40 A20 20 0 1 0 34.14 54.14", 40),
    "t": ("M7 6 V48 A12 12 0 0 0 19 60 H22 M0 20 H22", 22),
    "o": ("M20 20 A20 20 0 0 1 20 60 A20 20 0 0 1 20 20 Z", 40),
    "v": ("M0 20 L18 60 L36 20", 36),
    "s": ("M26.12 25 A14 10 0 1 0 14 40 A14 10 0 1 1 1.88 55", 28),
}
# Each letter's origin: the spacing is optical (a round side or the r's open foot sits closer).
WORD = [("r", 0), ("e", 42), ("t", 104), ("r", 150), ("o", 192),
        ("v", 254), ("e", 310), ("r", 374), ("s", 416), ("e", 468)]
RETRO_LETTERS = 5

# The symbol's frame in a lockup, in wordmark units: two thirds of the tube.
LOCKUP_FRAME = 5.1


class Item:
    """One path of a composition: its centerline, an optional fill, an optional stroke, and the placement
    (uniform scale, then translation) of its grid in the composition's."""

    def __init__(self, d, stroke=None, width=0.0, fill=None, scale=1.0, tx=0.0, ty=0.0):
        self.d, self.stroke, self.width, self.fill = d, stroke, width, fill
        self.scale, self.tx, self.ty = scale, tx, ty


def wordmark(tx=0.0, ty=0.0, light=False):
    items = []
    for i, (letter, x) in enumerate(WORD):
        if i < RETRO_LETTERS:
            color = WALNUT if light else AMBER
        else:
            color = PLUM if light else MAGENTA
        items.append(Item(GLYPHS[letter][0], stroke=color, width=TUBE, tx=tx + x, ty=ty))
    return items


def lockup_mark(scale, tx, ty, light=False):
    return [Item(SCREEN, stroke=WALNUT if light else AMBER, width=LOCKUP_FRAME / scale, scale=scale, tx=tx, ty=ty),
            Item(R_LARGE, stroke=PLUM if light else MAGENTA, width=TUBE / scale, scale=scale, tx=tx, ty=ty)]


def small_icon(size):
    """The icon drawn on the pixel grid of a small size, in pixels (the item scales them to the 64-unit grid): a frame
    of whole pixels (1, 2 at 32 px) with straight sides (the bulge would be under a pixel), and an R whose strokes
    (2 pixels, 3 from 24 px) cover whole pixels, a row of air between it and the frame."""
    h = round(size * 3 / 4)
    top = (size - h) // 2
    line = 1 if size < 32 else 2
    x0, x1, y0, y1 = line / 2, size - line / 2, top + line / 2, top + h - line / 2
    rc = size / 10
    n = fmt
    frame = (f"M{n(x0 + rc)} {n(y0)} H{n(x1 - rc)} A{n(rc)} {n(rc)} 0 0 1 {n(x1)} {n(y0 + rc)} "
             f"V{n(y1 - rc)} A{n(rc)} {n(rc)} 0 0 1 {n(x1 - rc)} {n(y1)} "
             f"H{n(x0 + rc)} A{n(rc)} {n(rc)} 0 0 1 {n(x0)} {n(y1 - rc)} "
             f"V{n(y0 + rc)} A{n(rc)} {n(rc)} 0 0 1 {n(x0 + rc)} {n(y0)} Z")
    stroke = 2 if size < 24 else 3
    half = stroke / 2
    first, rows = top + line + 1, h - 2 * line - 2
    width = round(rows * 0.75)
    width -= (size - width) % 2
    left = (size - width) // 2
    stem, right = left + half, left + width - half
    y_top, y_end = first + half, first + rows - half
    y_mid = y_top + round((rows - stroke) / 2)
    radius = (y_mid - y_top) / 2
    bowl = right - radius
    leg = bowl - round(radius / 3)
    letter = (f"M{n(stem)} {n(y_end)} V{n(y_top)} H{n(bowl)} A{n(radius)} {n(radius)} 0 0 1 {n(bowl)} {n(y_mid)} "
              f"H{n(stem)} M{n(leg)} {n(y_mid)} L{n(right)} {n(y_end)}")
    scale = 64 / size
    return [Item(frame, stroke=AMBER, width=line, fill=GLASS, scale=scale),
            Item(letter, stroke=MAGENTA, width=stroke, scale=scale)]


# The compositions. The symbol and the icons keep the 64-unit grid as their view box; the others are cropped to the
# drawing. Horizontal lockup: the screen 1.5 times the symbol, centered on the x-height; stacked: twice the symbol,
# centered over the wordmark.
SYMBOL = [Item(SCREEN, stroke=AMBER, width=3.2), Item(R_LARGE, stroke=MAGENTA, width=5.0)]
ICON_LARGE = [Item(SCREEN, stroke=AMBER, width=3.4, fill=GLASS), Item(R_LARGE, stroke=MAGENTA, width=5.0)]
LOCKUP = lockup_mark(1.5, 0, -8) + wordmark(122, 0)
LOCKUP_LIGHT = lockup_mark(1.5, 0, -8, light=True) + wordmark(122, 0, light=True)
LOCKUP_STACKED = lockup_mark(2.0, 190, 0) + wordmark(0, 118)

# The icon's sizes (Windows asks for 16 to 48 at 100 to 300% scaling, 256 for the large views); up to SMALL_DESIGN_MAX
# they are drawn on their own pixel grid.
ICON_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SMALL_DESIGN_MAX = 32


def svgs():
    """The SVG masters: name, items, view box ("grid": the symbol's 64 units; "crop": the drawing's extent)."""
    return [
        ("retroverse-symbol.svg", SYMBOL, "grid"),
        ("retroverse-icon.svg", ICON_LARGE, "grid"),
        ("retroverse-icon-16.svg", small_icon(16), "grid"),
        ("retroverse-wordmark.svg", wordmark(), "crop"),
        ("retroverse-lockup.svg", LOCKUP, "crop"),
        ("retroverse-lockup-light.svg", LOCKUP_LIGHT, "crop"),
        ("retroverse-lockup-stacked.svg", LOCKUP_STACKED, "crop"),
    ]


# ---------------------------------------------------------------- paths

TOKEN = re.compile(r"[MLHVQAZ]|-?(?:\d+\.?\d*|\.\d+)")


def flatten(d):
    """The polylines of an absolute path of M, L, H, V, Q, A and Z commands, in its own units, with whether each is
    closed. Quadratic curves in 24 chords, arcs in chords of at most 2 degrees."""
    tokens = TOKEN.findall(d)
    pos = 0
    lines, cur = [], None
    x = y = sx = sy = 0.0

    def num():
        nonlocal pos
        value = float(tokens[pos])
        pos += 1
        return value

    while pos < len(tokens):
        cmd = tokens[pos]
        pos += 1
        if cmd == "M":
            x, y = num(), num()
            sx, sy = x, y
            cur = [(x, y)]
            lines.append([cur, False])
        elif cmd == "L":
            x, y = num(), num()
            cur.append((x, y))
        elif cmd == "H":
            x = num()
            cur.append((x, y))
        elif cmd == "V":
            y = num()
            cur.append((x, y))
        elif cmd == "Q":
            cx, cy, ex, ey = num(), num(), num(), num()
            for k in range(1, 25):
                t = k / 24
                u = 1 - t
                cur.append((u * u * x + 2 * u * t * cx + t * t * ex, u * u * y + 2 * u * t * cy + t * t * ey))
            x, y = ex, ey
        elif cmd == "A":
            rx, ry, _rotation, large, sweep, ex, ey = num(), num(), num(), num(), num(), num(), num()
            cur.extend(arc_points(x, y, rx, ry, large != 0, sweep != 0, ex, ey))
            x, y = ex, ey
        elif cmd == "Z":
            if cur[-1] != (sx, sy):
                cur.append((sx, sy))
            lines[-1][1] = True
            x, y = sx, sy
        else:
            raise ValueError(f"unsupported path command {cmd!r}")
    return lines


def arc_points(x1, y1, rx, ry, large, sweep, x2, y2):
    """An unrotated elliptical arc's points after its start (SVG 1.1 appendix F.6.5, endpoint to center)."""
    hx, hy = (x1 - x2) / 2, (y1 - y2) / 2
    scale = (hx * hx) / (rx * rx) + (hy * hy) / (ry * ry)
    if scale > 1:
        rx, ry = rx * math.sqrt(scale), ry * math.sqrt(scale)
    num = rx * rx * ry * ry - rx * rx * hy * hy - ry * ry * hx * hx
    den = rx * rx * hy * hy + ry * ry * hx * hx
    coef = math.sqrt(max(0.0, num / den)) * (-1 if large == sweep else 1)
    ccx, ccy = coef * rx * hy / ry, -coef * ry * hx / rx
    cx, cy = ccx + (x1 + x2) / 2, ccy + (y1 + y2) / 2
    theta1 = math.atan2((hy - ccy) / ry, (hx - ccx) / rx)
    theta2 = math.atan2((-hy - ccy) / ry, (-hx - ccx) / rx)
    delta = theta2 - theta1
    if sweep and delta < 0:
        delta += 2 * math.pi
    elif not sweep and delta > 0:
        delta -= 2 * math.pi
    steps = max(1, math.ceil(abs(delta) / math.radians(2)))
    points = [(cx + rx * math.cos(theta1 + delta * k / steps), cy + ry * math.sin(theta1 + delta * k / steps))
              for k in range(1, steps)]
    points.append((x2, y2))
    return points


def placed(item, factor=1.0):
    """The item's polylines in the composition's units, multiplied by factor (pixels per unit)."""
    s = item.scale * factor
    return [([(px * s + item.tx * factor, py * s + item.ty * factor) for px, py in line], closed)
            for line, closed in flatten(item.d)]


def bounds(items):
    """The drawing's extent, strokes included, rounded outwards to whole units."""
    xs, ys = [], []
    for item in items:
        half = item.width * item.scale / 2 if item.stroke else 0.0
        for line, _closed in placed(item):
            for px, py in line:
                xs.extend((px - half, px + half))
                ys.extend((py - half, py + half))
    return math.floor(min(xs)), math.floor(min(ys)), math.ceil(max(xs)), math.ceil(max(ys))


# ---------------------------------------------------------------- SVG

def fmt(value):
    text = f"{value:.3f}".rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


def svg(items, box):
    if box == "grid":
        x0, y0, x1, y1 = 0, 0, 64, 64
    else:
        x0, y0, x1, y1 = bounds(items)
    w, h = x1 - x0, y1 - y0
    out = ['<svg xmlns="http://www.w3.org/2000/svg" '
           f'viewBox="{x0} {y0} {w} {h}" width="{w}" height="{h}">',
           "  <title>Retroverse</title>",
           '  <g fill="none" stroke-linecap="round" stroke-linejoin="round">']
    for item in items:
        attrs = [f'd="{item.d}"']
        transform = []
        if item.tx or item.ty:
            transform.append(f"translate({fmt(item.tx)} {fmt(item.ty)})")
        if item.scale != 1.0:
            transform.append(f"scale({fmt(item.scale)})")
        if transform:
            attrs.append('transform="' + " ".join(transform) + '"')
        if item.fill:
            attrs.append(f'fill="{item.fill}"')
        if item.stroke:
            attrs.append(f'stroke="{item.stroke}" stroke-width="{fmt(item.width)}"')
        out.append(f"    <path {' '.join(attrs)}/>")
    out += ["  </g>", "</svg>", ""]
    return "\n".join(out).encode("utf-8")


# ---------------------------------------------------------------- raster

def rgb(color):
    return tuple(int(color[i:i + 2], 16) / 255 for i in (1, 3, 5))


def segments(lines):
    segs = []
    for line, _closed in lines:
        for (ax, ay), (bx, by) in zip(line, line[1:]):
            dx, dy = bx - ax, by - ay
            segs.append((ax, ay, dx, dy, dx * dx + dy * dy, min(ax, bx), max(ax, bx), min(ay, by), max(ay, by)))
    return segs


def distance2(px, py, seg):
    ax, ay, dx, dy, length2 = seg[:5]
    t = 0.0 if length2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length2))
    ex, ey = px - ax - t * dx, py - ay - t * dy
    return ex * ex + ey * ey


def stroke_coverage(lines, half, size):
    """Per pixel, how much of it the stroke covers: its half width plus half a pixel minus the distance of the
    pixel's center to the centerline, clamped (and never more than the stroke's width, for hairlines)."""
    reach = half + 1.0
    segs = segments(lines)
    cover = [0.0] * (size * size)
    for j in range(size):
        py = j + 0.5
        row = [s for s in segs if s[7] - reach <= py <= s[8] + reach]
        if not row:
            continue
        for i in range(size):
            px = i + 0.5
            best = math.inf
            for s in row:
                if s[5] - reach <= px <= s[6] + reach:
                    best = min(best, distance2(px, py, s))
            if best < math.inf:
                cover[j * size + i] = max(0.0, min(1.0, 2 * half, half + 0.5 - math.sqrt(best)))
    return cover


def fill_coverage(lines, size):
    """Per pixel, how much of it the closed outline's inside covers: half a pixel plus the signed distance of the
    pixel's center to the outline, clamped; inside by the even-odd rule."""
    segs = segments(lines)
    cover = [0.0] * (size * size)
    for j in range(size):
        py = j + 0.5
        crossings = sorted(s[0] + (py - s[1]) / s[3] * s[2] for s in segs
                           if s[3] != 0 and min(s[1], s[1] + s[3]) <= py < max(s[1], s[1] + s[3]))
        near = [s for s in segs if s[7] - 1 <= py <= s[8] + 1]
        for i in range(size):
            px = i + 0.5
            inside = sum(1 for c in crossings if c < px) % 2 == 1
            best = math.inf
            for s in near:
                if s[5] - 1 <= px <= s[6] + 1:
                    best = min(best, distance2(px, py, s))
            if best < math.inf:
                signed = math.sqrt(best) * (1 if inside else -1)
                cover[j * size + i] = max(0.0, min(1.0, 0.5 + signed))
            elif inside:
                cover[j * size + i] = 1.0
    return cover


def rasterize(items, size, grid=64):
    """The composition as RGBA bytes, rows from the top: each item's fill, then its stroke, over transparency."""
    factor = size / grid
    pr, pg, pb, pa = ([0.0] * (size * size) for _ in range(4))

    def over(cover, color):
        cr, cg, cb = rgb(color)
        for k, a in enumerate(cover):
            if a > 0:
                keep = 1 - a
                pr[k] = cr * a + pr[k] * keep
                pg[k] = cg * a + pg[k] * keep
                pb[k] = cb * a + pb[k] * keep
                pa[k] = a + pa[k] * keep

    for item in items:
        lines = placed(item, factor)
        if item.fill:
            over(fill_coverage(lines, size), item.fill)
        if item.stroke:
            over(stroke_coverage(lines, item.width * item.scale * factor / 2, size), item.stroke)

    out = bytearray()
    for k in range(size * size):
        a = pa[k]
        if a <= 0:
            out += b"\0\0\0\0"
            continue
        out += bytes(min(255, int(c / a * 255 + 0.5)) for c in (pr[k], pg[k], pb[k]))
        out.append(min(255, int(a * 255 + 0.5)))
    return bytes(out)


def png(rgba, width, height):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    stride = width * 4
    raw = b"".join(b"\0" + rgba[y * stride:(y + 1) * stride] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib(rgba, size):
    """A 32-bit icon bitmap: the header (twice the height, for the mask), BGRA rows from the bottom, and the AND mask
    (1 where the pixel is fully transparent) in rows padded to 32 bits."""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = bytearray()
    mask = bytearray()
    mask_stride = (size + 31) // 32 * 4
    for y in reversed(range(size)):
        row_mask = bytearray(mask_stride)
        for x in range(size):
            r, g, b, a = rgba[(y * size + x) * 4:(y * size + x) * 4 + 4]
            pixels += bytes((b, g, r, a))
            if a == 0:
                row_mask[x // 8] |= 0x80 >> (x % 8)
        mask += row_mask
    return header + bytes(pixels) + bytes(mask)


def icon_images():
    return {size: rasterize(small_icon(size) if size <= SMALL_DESIGN_MAX else ICON_LARGE, size)
            for size in ICON_SIZES}


def ico(images):
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for size, rgba in images.items():
        blob = png(rgba, size, size) if size >= 256 else dib(rgba, size)
        side = 0 if size >= 256 else size
        entries.append(struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    return struct.pack("<HHH", 0, 1, len(images)) + b"".join(entries) + b"".join(blobs)


def ico_pixels(data):
    """An icon's images as (size, pixel bytes): a PNG's decompressed rows, a bitmap as stored. --check compares these,
    so that another zlib's compression of the same pixels still passes."""
    _reserved, _kind, count = struct.unpack_from("<HHH", data, 0)
    result = []
    for k in range(count):
        side, _h, _c, _r, _p, _bits, length, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * k)
        blob = data[offset:offset + length]
        if blob.startswith(b"\x89PNG"):
            pos, idat = 8, b""
            while pos < len(blob):
                (n,) = struct.unpack_from(">I", blob, pos)
                if blob[pos + 4:pos + 8] == b"IDAT":
                    idat += blob[pos + 8:pos + 8 + n]
                pos += 12 + n
            blob = zlib.decompress(idat)
        result.append((side or 256, blob))
    return result


def enlarged(rgba, size, factor, ground=(0x17, 0x1B, 0x22)):
    """The image over a ground, each pixel a factor x factor block: to look at the small sizes."""
    out = bytearray()
    for y in range(size):
        row = bytearray()
        for x in range(size):
            r, g, b, a = rgba[(y * size + x) * 4:(y * size + x) * 4 + 4]
            mixed = bytes(int((c * a + ground[i] * (255 - a)) / 255 + 0.5) for i, c in enumerate((r, g, b)))
            row += (mixed + b"\xff") * factor
        out += bytes(row) * factor
    return png(bytes(out), size * factor, size * factor)


# ---------------------------------------------------------------- main

def outputs(images):
    files = {name: svg(items, box) for name, items, box in svgs()}
    files["retroverse.ico"] = ico(images)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--check", action="store_true", help="compare with the committed files, write nothing")
    parser.add_argument("--preview", type=Path, help="also write the icon's sizes as PNG into this directory")
    args = parser.parse_args()

    images = icon_images()
    files = outputs(images)
    if args.check:
        stale = []
        for name, data in files.items():
            path = OUT / name
            if not path.is_file():
                stale.append(f"{name}: missing")
                continue
            present = path.read_bytes()
            same = ico_pixels(present) == ico_pixels(data) if name.endswith(".ico") else present == data
            if not same:
                stale.append(f"{name}: differs")
        for line in stale:
            print(line, file=sys.stderr)
        print("branding: " + ("stale, run python -I scripts/branding.py" if stale else "up to date"))
        return 1 if stale else 0

    OUT.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (OUT / name).write_bytes(data)
        print(f"wrote assets/branding/{name} ({len(data)} bytes)")
    if args.preview:
        args.preview.mkdir(parents=True, exist_ok=True)
        for size, rgba in images.items():
            (args.preview / f"icon-{size}.png").write_bytes(png(rgba, size, size))
            if size <= 48:
                (args.preview / f"icon-{size}-x8.png").write_bytes(enlarged(rgba, size, 8))
        print(f"wrote the icon's previews into {args.preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
