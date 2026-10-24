# ADR-0014: The styling pass of the building

- Status: accepted
- Date: 2026-10-24
- Amends:
  - ADR-0013 §3 and §5: the theme pack is no longer sticky, and media families gain a one-time successor migration;
  - ADR-0013 §6 and §7: the ground floor's lobby becomes the hub, its rooms re-placed once;
  - ADR-0013 §9: lights reach an area through its doorways;
  - ADR-0010 §10: the migration is the one exception to "nothing that stands moves", announced and reported.
- Relates to: style.md §3, §4, §5.2-§5.5, §8, §14, §17, §26-§28; `example.png`; roadmap I6 (Gate D)

## Context

I4 built a functional building (ADR-0013); its look was left for a styling pass.

**The building today (`docs/testing/gate-c.md`):**
- **Halls:** plain boxes of fixed materials, dim, with a light every 6 m.
- **Rooms seen through a doorway:** black, because only the eye's area is lit.
- **Lobby:** a 6 m-wide corridor.
- **Theme:** one pack for the whole building.
- **Cases and boxes:** they stand with their height in the shelf's depth, and face-out ones show their spine label
  upside down on top.
- **Arcade room:** a CRT where a cabinet should stand.

**What the documents ask for:**
- **The concept (`example.png`):** a warm refuge-museum, with dark wood, tungsten light and one neon accent per room.
- **The hub (style §5.2):** two storeys, with a glowing landmark and signed portals.
- **The theme packs (style §14):** one per era.
- **Arcade and handhelds (§5.4, §5.5):** cabinets in the arcade, low showcases for handhelds.

The user chose, on 2026-10-24, the whole pass:
- the hub;
- halls and light;
- theme packs per floor;
- arcade and handhelds;
- side-face spines for cases and boxes.

The pass is closed by Gate D.

## Decision

### 1. The theme pack is a look, not a plan fact

A theme pack chooses materials, plastics, label palettes, neon colours, light accents and decoration weights. It never
chooses geometry, slots or placements.

- **Which pack.** A room's pack is resolved when its scene is built: its floor's pack (`building.json`), else its
  kind's default (`systems.json`), else a system's platform override.
- **The stored field.** `Room.themePack` stays in the plan, but an update writes the resolved pack into it. This is
  never a reason to keep or regenerate a room, and it moves no placement.
- **What this replaces.** ADR-0013 §3's "the recorded theme pack is authoritative" is withdrawn for the theme pack
  only. The family stays recorded (§2 below).

### 2. Side-face spines: successor families, migrated once

Cases and boxes stand spine-out on their side (`SlotFacing::SideOut`): the item is turned a quarter about +Y, so its
spine face, the item's -X face, looks out, carrying an upright spine label. Their slot is (d, h, w).

- **New family ids.** A family is never redefined (ADR-0013 §4), so these get new ids, each naming its predecessor:
  - `disc_case_cd_v2`;
  - `disc_case_dvd_v2`;
  - `case_handheld_v2`;
  - `big_box_v2`;
  - `generic_case_v2`.
- **New arcade family.** The arcade room's collection becomes the family `arcade_cabinet`, whose fixtures are rows of
  cabinets with one face-out slot each. It succeeds `generic_case` in the arcade room only.
- **The migration.** A room whose recorded family has a successor is regenerated once, room by room, at the first
  update by this generator:
  - it keeps its id, floor, x, sections and seed;
  - it chooses its face-out titles again by the same priority (ADR-0010 §3).
- **Reporting.** The change report lists every migrated room. The run report records them. The app shows a one-time
  notice.
- **What stays put.** Every other room is untouched, byte for byte in its elements. Cartridge families have no
  successor.
- **Scope.** This is the one exception to ADR-0010 §10's "nothing that stands moves". It is made once, for this pass,
  and announced; it is never a precedent for silent moves.
- **Face-out items show no spine label.** An instance flag discards the spine label's fragments on face-out items. The
  shell's top lies beneath it, so no hole shows.

### 3. The ground floor is the hub

The ground floor's derived hall becomes the hub (style §5.2, ADR-0013's lobby): about 18 x 14 m and 5 m high. It holds:
- columns and beams, and a decorative balcony that cannot be walked;
- a central landmark: a glowing globe on a pedestal in a railed circular showcase of generic consoles;
- signed portals to the elevator, the arcade room and the closed entrance;
- the directory's totem, a proximity zone like the elevator's;
- one or two information screens;
- seating, rugs and plants;
- a showcase of favourites and recent games, as runtime proxies, never plan placements (ADR-0013 §10).

**Re-placement.** Rooms on the ground floor are re-placed once onto the hub's walls: new x and z, their contents
unchanged in their own frames. The hub is derived, not stored, as halls are (ADR-0013 §5).

### 4. Light through doorways

An area's light selection adds, after its own lights and within the 16:
- up to 4 lights of each neighbouring area (a room and its hall touch at the doorway passage);
- only those within a short range of the shared passage.

A neighbour's light is never the key light and casts no shadow.

The scene adds, for each doorway, a low fill light in the hall aimed into the room. Each area has its own environment
(exposure, ambient, fill), eased over about 0.5 s when the eye changes area. So upper walls and ceilings no longer fall
to black, and a dark arcade can sit beside a bright hub.

### 5. Theme packs per floor

| Floor | Pack |
|---|---|
| `ground` (the hub) | `retro_library_hub` |
| `gen2`, `gen3`, `gen4` | `classic_16bit_warm` |
| `gen5` | `console_90s_collector` |
| `gen6`, `gen7` | `optical_era_lounge` |
| `handheld` | `handheld_archive` |
| `computer` | `computer_studio` (new; style §14 names none for computers) |
| the arcade room | `arcade_after_dark` (by its kind, wherever it stands) |

- **The palette.** The 70/20/10 rule (style §3.2) binds every pack: 70% neutral warm materials, 20% the era's colour,
  10% light accents. Each room gets a neon sign in its pack's accent colour, with lit shelf interiors.
- **Signs.** They stay generic texts, never trade names (ADR-0013 §1).

### 6. Arcade and handhelds

- **The arcade.** The `arcade_cabinet` prefab is an upright body with an emissive marquee and a `ScreenSurface`. In the
  arcade room:
  - the hero corner's CRT becomes a cabinet carrying the video, as a part substitution by room kind, so the template
    file does not change;
  - each face-out slot is a cabinet whose screen shows the game's cover or screenshot.
- **The handhelds.** The handheld families' fixtures gain low counter showcases. Handheld rooms gain stations: a
  generic console on a stand.

## Amendments

- **ADR-0013 §3:** the recorded theme pack is no longer authoritative (§1 above). The recorded family stays.
- **ADR-0013 §4:** families gain successors with a one-time migration (§2 above).
- **ADR-0013 §6 and §7:** the ground floor's hall is the hub; its rooms are re-placed once (§3 above).
- **ADR-0013 §9:** neighbour lights through doorways, and an environment per area (§4 above).
- **Roadmap:** I6 is the styling pass, done before I5 and I4b.

## Consequences

**Gains**
- The building reads like the concept: warm, varied by era, with a real hub, an arcade and readable cases.

**Costs**
- One visible reshuffle of case, box and arcade rooms in a stored building, reported.
- Every room golden is regenerated on purpose, and plan goldens change at steps 4 and 5.
- Light selection is a little more work per area change. It stays within 16 lights per view and the acceptable rows.

**Deferred**
- Stairs that can be walked.
- L-shaped templates and layout candidates.
- Faithful per-console families beyond the successors.
- Video on more screens (I5).
- Blender hero assets (I8).
