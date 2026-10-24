# I6: the look of the building, floor by floor

The direction the styling pass follows ([ADR-0014](../decisions/ADR-0014-the-styling-pass.md)), drawn from
`example.png` and style.md. Every step compares its screenshots with the panel named here.

## Rules for every floor

- **Palette.** The palette of style §3:
  - walnut `#493024` and oak `#76513A`;
  - walls `#383735`;
  - graphite `#252A2E`;
  - bordeaux `#683D42`;
  - tungsten `#FFC88A`;
  - cold accent `#4C8BC9`;
  - arcade magenta `#B34DCE`;
  - signage green `#6EAF93`.

  Apply it 70/20/10: neutral warm materials, then the era's colour, then light accents. Coloured light never replaces
  geometry.
- **Light (style §4).** Warm lamps and screens that are really on:
  - five layers: fill, practicals, accents on the shelves and cases, emissives, contact shadows;
  - upper walls and ceilings never fall to black;
  - one neon sign per room, in its pack's accent.
- **Density.** Cozy and curated (`decoration_density` 0.25 to 0.60, style §6). Keep the passages at 1.4 m, nothing on
  the main axis and some negative space. Light and signs keep an alternating rhythm (style §13.4).
- **Signs (style §17).** Generic texts drawn from text, never trade names.
  - Hall level: the portal above each doorway carries its room's `sign`.
  - Room level: a neon panel.
  - Object level: discreet.
- **Shelves.** Lit inside (an emissive strip under each board). Cases and boxes stand on their side with an upright
  spine (ADR-0014 §2).

## Floor by floor

| Floor | Pack | Concept panel | Materials | Accent | Furniture and dressing |
|---|---|---|---|---|---|
| `ground`, the hub | `retro_library_hub` | 1 | walnut columns and beams, oak floor, Persian rugs, graphite railings | amber, faint blue, small red and green on the portals | the landmark globe in its railed showcase, signed portals (elevator, arcade, entrance), the directory's totem, a favourites showcase, sofas, plants, wall sconces, the balcony |
| `gen2` to `gen4` | `classic_16bit_warm` | 2 | parquet, dark bookcases, bordeaux fabric | magenta, red | CRT on its cabinet, rug, armchair, table lamp, posters above the shelves, plants |
| `gen5` | `console_90s_collector` | (2 and 4) | warm wood with colourful 90s geometric posters | soft green, blue, red | mixed shelving with glass cases, a beanbag or low sofa |
| `gen6`, `gen7` | `optical_era_lounge` | 4 | dark walls, graphite metal, glass | discreet blue (`#4C8BC9`) with warm lamps for contrast | a sofa, a glass display case, large framed posters, a blue rug |
| `handheld` | `handheld_archive` | 6 | oak shelves, glass | soft green, blue | low counter showcases, stations (a console on a stand), a small lounge |
| `computer` | `computer_studio` | (none: a study) | oak desks, green-shaded lamps, cork boards | signage green | desks with a monitor and keyboard, bookcases of boxes, chairs |
| the arcade room | `arcade_after_dark` | 5 | dark base, red-patterned runner | magenta, cyan, red, more bloom | rows of cabinets with lit marquees, stools, one light per cabinet, clear space in front of each |

## Halls

The halls have their own role in each pack (`hall`): floor, wall, ceiling, trim, baseboard and runner.
- **Lights:** warm sconces alternating with pictures.
- **Furniture:** benches and plants at intervals.
- **Architecture:** a beam at each doorway, and a portal with the room's sign over each doorway.

The halls take their floor's pack.

## What the screenshots must show (Gate D)

- **The hub:** the landmark readable on entry, the portals signed, warm and grand but welcoming.
- **A hall:** looking down it, rooms glow through their doorways, never black holes.
- **A room of each pack:** recognisably its era next to the concept panel, with the shelves lit and the spines upright.
- **The arcade:** cabinets in rows, their marquees lit, darker than the hub.
- **The handhelds:** low lit showcases at viewing height.
