"""Writes the glTF 2.0 test models of the --model viewer checks (CLAUDE.md, "Visual verification") into a directory,
normally build/models (git-ignored; never commit generated models):

- orientation_cube.gltf (+ .bin, .png): a 0.5 m cube standing on the origin whose six faces show labeled tiles of one
  texture atlas (FRONT +Z, RIGHT +X, TOP +Y, BACK -Z, LEFT -X, BOTTOM -Y), with Anchor_Top and Anchor_Front sockets.
  Every tile reads upright from outside the cube, has an arrow pointing to its top edge and a dark square in its
  top-left corner, so a mirrored or flipped texture, a swapped axis or a wrong winding shows at a glance.
- orientation_cube_1cm.gltf: the same cube scaled to 1 cm (framing, clip planes and the millimeter grid).
- material_swatches.gltf: four 0.3 m cubes in a row along +X with the base colors and roughness of MAT_Plastic_Black,
  MAT_Metal_Graphite, MAT_Plastic_Gray and MAT_Plastic_AgedIvory (assets/materials/base/materials.json): every
  silhouette, the darkest included, must stand out from the viewer's background.

Usage (Python 3 with Pillow >= 10.1, for the label font):
    python -I scripts/make_test_models.py build/models
"""

import json
import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("make_test_models.py needs Pillow >= 10.1: python -m pip install pillow")

TILE = 256
COLUMNS, ROWS = 3, 2
HALF = 0.25  # half edge of the orientation cube, meters

# (label, axis, tile column, tile row, background, outward normal, right, up) as seen from outside the face.
FACES = [
    ("FRONT", "+Z", 0, 0, (70, 130, 220), (0, 0, 1), (1, 0, 0), (0, 1, 0)),
    ("RIGHT", "+X", 1, 0, (210, 70, 60), (1, 0, 0), (0, 0, -1), (0, 1, 0)),
    ("TOP", "+Y", 2, 0, (70, 170, 80), (0, 1, 0), (1, 0, 0), (0, 0, -1)),
    ("BACK", "-Z", 0, 1, (40, 60, 110), (0, 0, -1), (-1, 0, 0), (0, 1, 0)),
    ("LEFT", "-X", 1, 1, (110, 40, 35), (-1, 0, 0), (0, 0, 1), (0, 1, 0)),
    ("BOTTOM", "-Y", 2, 1, (35, 90, 45), (0, -1, 0), (1, 0, 0), (0, 0, 1)),
]

# Material presets of the swatches: (name, sRGB base color, roughness), darkest first.
SWATCHES = [
    ("MAT_Plastic_Black", "#1C1C1E", 0.42),
    ("MAT_Metal_Graphite", "#252A2E", 0.45),
    ("MAT_Plastic_Gray", "#9A9A96", 0.5),
    ("MAT_Plastic_AgedIvory", "#D6CBAE", 0.48),
]
SWATCH_EDGE = 0.3  # meters
SWATCH_STEP = 0.45  # meters between swatch centers


def make_atlas(path: Path) -> None:
    atlas = Image.new("RGB", (TILE * COLUMNS, TILE * ROWS), (0, 0, 0))
    draw = ImageDraw.Draw(atlas)
    big = ImageFont.load_default(size=54)
    small = ImageFont.load_default(size=40)
    for label, axis, column, row, background, *_ in FACES:
        x0, y0 = column * TILE, row * TILE
        draw.rectangle([x0, y0, x0 + TILE - 1, y0 + TILE - 1], fill=background, outline=(240, 240, 240), width=6)
        # Top-left marker and an arrow towards the top edge.
        draw.rectangle([x0 + 14, y0 + 14, x0 + 54, y0 + 54], fill=(20, 20, 20))
        cx = x0 + TILE // 2
        draw.polygon([(cx, y0 + 18), (cx - 26, y0 + 58), (cx + 26, y0 + 58)], fill=(250, 250, 250))
        draw.rectangle([cx - 8, y0 + 56, cx + 8, y0 + 92], fill=(250, 250, 250))
        draw.text((cx, y0 + 150), label, font=big, fill=(255, 255, 255), anchor="mm")
        draw.text((cx, y0 + 208), axis, font=small, fill=(255, 255, 220), anchor="mm")
    atlas.save(path)


def add(a, b, scale=1.0):
    return tuple(x + y * scale for x, y in zip(a, b))


def make_mesh():
    """Unit-scaled cube of edge 2 * HALF around the origin: 4 vertices per face, CCW from outside, uv per atlas tile."""
    positions, normals, uvs, indices = [], [], [], []
    for _, _, column, row, _, normal, right, up in FACES:
        center = tuple(HALF * n for n in normal)
        corners = [
            add(add(center, right, -HALF), up, HALF),  # top-left
            add(add(center, right, HALF), up, HALF),  # top-right
            add(add(center, right, HALF), up, -HALF),  # bottom-right
            add(add(center, right, -HALF), up, -HALF),  # bottom-left
        ]
        u0, u1 = column / COLUMNS, (column + 1) / COLUMNS
        v0, v1 = row / ROWS, (row + 1) / ROWS  # glTF: v grows downwards from the image's top row
        base = len(positions)
        positions += corners
        normals += [normal] * 4
        uvs += [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]
        # Counter-clockwise seen from outside: top-left, bottom-left, bottom-right, then top-left, bottom-right, top-right.
        indices += [base, base + 3, base + 2, base, base + 2, base + 1]
    return positions, normals, uvs, indices


def write_buffer(path: Path):
    """Writes the cube's vertex and index data; returns the buffer, buffer views and accessors that describe it."""
    positions, normals, uvs, indices = make_mesh()
    blob = bytearray()
    views = []

    def append(data: bytes, target: int) -> int:
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
        blob.extend(data)
        return len(views) - 1

    pos_view = append(b"".join(struct.pack("<3f", *p) for p in positions), 34962)
    nrm_view = append(b"".join(struct.pack("<3f", *n) for n in normals), 34962)
    uv_view = append(b"".join(struct.pack("<2f", *t) for t in uvs), 34962)
    idx_view = append(b"".join(struct.pack("<H", i) for i in indices), 34963)
    path.write_bytes(bytes(blob))
    accessors = [
        {"bufferView": pos_view, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": [-HALF, -HALF, -HALF], "max": [HALF, HALF, HALF]},
        {"bufferView": nrm_view, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": uv_view, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
        {"bufferView": idx_view, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
    ]
    buffers = [{"uri": path.name, "byteLength": len(blob)}]
    return buffers, views, accessors


def primitive(material: int) -> dict:
    return {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": material}


def srgb_to_linear(hex_color: str) -> list:
    channels = [int(hex_color[i:i + 2], 16) / 255.0 for i in (1, 3, 5)]
    return [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]


def write_gltf(path: Path, document: dict) -> None:
    document["asset"] = {"version": "2.0", "generator": "Retroverse test models (scripts/make_test_models.py)"}
    path.write_text(json.dumps(document, indent=2), encoding="utf-8")
    print(f"wrote {path}")


def orientation_cube(buffers, views, accessors, scale: float) -> dict:
    root = {"name": "OrientationCube", "translation": [0.0, HALF * scale, 0.0], "mesh": 0, "children": [1, 2]}
    if scale != 1.0:
        root["scale"] = [scale, scale, scale]
    return {
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            root,
            {"name": "Anchor_Top", "translation": [0.0, HALF, 0.0]},
            {"name": "Anchor_Front", "translation": [0.0, 0.0, HALF]},
        ],
        "meshes": [{"name": "cube", "primitives": [primitive(0)]}],
        "materials": [{"name": "OrientationAtlas", "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.6}}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"uri": "orientation_cube.png"}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": buffers,
    }


def material_swatches(buffers, views, accessors) -> dict:
    scale = SWATCH_EDGE / (2.0 * HALF)
    first = -0.5 * SWATCH_STEP * (len(SWATCHES) - 1)
    nodes, meshes, materials = [], [], []
    for index, (name, color, roughness) in enumerate(SWATCHES):
        materials.append({"name": name, "pbrMetallicRoughness": {
            "baseColorFactor": srgb_to_linear(color) + [1.0], "metallicFactor": 0.0, "roughnessFactor": roughness}})
        meshes.append({"name": f"swatch_{index}", "primitives": [primitive(index)]})
        nodes.append({"name": name.removeprefix("MAT_"), "mesh": index, "scale": [scale, scale, scale],
                      "translation": [first + index * SWATCH_STEP, 0.5 * SWATCH_EDGE, 0.0]})
    return {
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": buffers,
    }


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    make_atlas(out / "orientation_cube.png")
    print(f"wrote {out / 'orientation_cube.png'}")
    buffers, views, accessors = write_buffer(out / "orientation_cube.bin")
    print(f"wrote {out / 'orientation_cube.bin'}")
    write_gltf(out / "orientation_cube.gltf", orientation_cube(buffers, views, accessors, 1.0))
    write_gltf(out / "orientation_cube_1cm.gltf", orientation_cube(buffers, views, accessors, 0.02))
    write_gltf(out / "material_swatches.gltf", material_swatches(buffers, views, accessors))


if __name__ == "__main__":
    main()
