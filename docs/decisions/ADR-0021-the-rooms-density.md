# ADR-0021: The rooms' density: dressing, the gallery of the user's art, atmosphere, sound and life

- Status: accepted (the user's choice of the first round's nine proposals and the 4 GiB memory budget on 2026-11-08:
  [i12/plan.md](../i12/plan.md), "Decisions made with the user")
- Date: 2026-11-08
- Amends:
  - the roadmap's "Acceptable performance": the GPU memory rows rise from 512 MiB to 4 GiB (§8);
  - ADR-0010 §2 (`WorldConfig`): the curated preset's decoration density rises from 0.4 to 0.65 for new and
    regenerated worlds (§3);
  - ADR-0013 ("Memory"), ADR-0016 §12 and ADR-0019 §9: their memory rows and remedies read against 4 GiB (§8);
  - ADR-0014 §1: what a theme pack may draw as a look grows from furniture variants within footprints to the room's
    dressing layer (§1), and `roomDressing` becomes every pack's, not two packs' (§1, §2);
  - ADR-0016 §9: the `effects` and `ambience` buses carry synthesized sounds from I12 (§5), so "original sounds with
    provenance (I8)" no longer gates them; the `crt_glow` accent takes the picture's average colour (§4);
  - ADR-0018 §2: `world.gallery`, `video.audio.ambience` and `video.audio.effects` are display-only keys of
    `settings.json`;
  - ADR-0019 §1: the user's game media (fanart, marquees, screenshots, title screens, miximages, 3D boxes) are shown
    in the room as the covers and the logos are: read at runtime, never copied into the repository or packaged (§2);
  - style §6.3: the "ricca" band (0.60 to 0.85) is the default for a room of any size, not only for large collections;
    §25: `DustMotes` and `NeonGlow` are built in this iteration (§4).
- Relates to: style.md §1.1, §1.2, §3, §4, §6, §13.4, §14, §15.4, §20, §25, §26.2; `example.png` panels 2, 4, 5, 6;
  `docs/testing/gate-d.md` ("Limits still open"); roadmap I12.

## Context

Gate D judged every room golden "the same warm museum of games", each era told apart, and recorded the one gap that
stayed open: the rooms are calmer than the concept, because the concept's colour and clutter come from commercial art
(walls of boxes, posters, marquees, lit game screens) that Retroverse does not ship. Since then I11 brought the real
logos, colours and console models into the rooms as the user's own content, and the room still uses three of the
twelve media kinds the catalog indexes (covers, clips, screenshots).

What a room shows today, from its doorway (`docs/testing/gate-d.md`, G02, G12, G15, G16, G17, G22):
- a 9 x 10 m box with a 3.2 m ceiling, bare above the shelves (a metre of plaster) and across the whole ceiling;
- two or four shelf modules and, in a room under a few hundred titles, side walls dressed with one low, small piece
  per socket (a poster at 1.55 m, a slim lamp, a narrow cabinet, a plant) at a metre's pitch, so the walls read bare;
- two low display cases at the edges of the circulation band on 9 m of empty parquet;
- a hero corner that is a diagram (cabinet, CRT, console, seat, rug, lamp, sign, plaque), not a lived-in set;
- nothing that moves or sounds besides the clips: no dust, no flicker, no hum, no footsteps; the `effects` and
  `ambience` buses exist and carry nothing.

The user asked, on 2026-11-08, to raise the single rooms' quality "brutally": denser and more immersive. Twelve
proposals were evaluated together; nine were chosen for a first round, in the order the eye reads a room from its
doorway, and three parked ([i12/plan.md](../i12/plan.md)). At the plan's review the user also set the GPU memory
budget at 4 GiB "from now on" (§8).

What binds every choice below:
- **ADR-0010 §10, "nothing that stands moves"**: no placement, slot or fixture of a stored plan changes; the one
  plan-shaping change (§3) applies to new worlds and to rooms the user regenerates, as I4b's regeneration already
  offers.
- **ADR-0014 §1**: a theme pack is a look and never geometry or slots; everything here is chosen when the scene is
  built, from the room's seed, or swapped in at runtime (I6 step 3b, I11 steps 4, 5 and 7).
- **ADR-0019**: trademarked and commercial content is the user's, read at runtime, never in `assets/`, fixtures,
  goldens or the package.
- **The acceptable rows** (roadmap): the benchmark path's and VR's frame rows, and the GPU memory rows at 4 GiB (§8).
- **The light budget**: 16 point and spot lights per view, one shadowed spot (the key); a real console room already
  draws 15 (the gen4 scene golden: key, pendant, the hero lamp, the sign's light, two washers, four decoration lamps,
  five accents).
- **`world.reduceMotion`**: every motion freezes under it (it is forced in VR), and every time-driven effect is a
  function of the simulation clock, so `--fixed-dt` keeps the goldens exact.

## Decision

### 1. The dressing layer: what a room draws beyond its plan

A room's scene is its plan (the shell, the fixtures, the collection, the lights) under a **dressing layer** chosen at
scene build from the room's seed (`core::deriveSeed(roomSeed, "<category>", <id>)`), the theme pack and the room's
data. ADR-0014 §1's furniture variants and I6 step 6's `roomDressing` are its first members; it now also holds:

- **The shell's details**: a skirting board, a picture rail at 2.25 m, the pack's wallpaper on the upper wall, a
  cornice under the ceiling and walnut beams on a 3 m grid (their undersides at 3.0 m), drawn as dressing primitives
  as the halls' trim is, never as new roles of the `room_shell` prefab (a role a pack does not list draws magenta);
  drum pendants hung along the room's axis; a cove strip on the upper walls where the pack asks for one (the lounge's
  at 2.98 m, above the gallery's band; the arcade keeps its neon coves). The room's interior, doorway and collision
  stay the plan's.
- **Look lights**: one look pendant light per base room on the axis towards the doorway, one more per two growth
  steps, drawn after the plan's lights and before the profile's and the pack's accents, so the selection drops
  accents first. The key spot stays the only shadow caster (a point light never casts one). Picture lights are faked:
  a hood and a wash quad fanning down over the picture, never a light.
- **The gallery** (§2): framed pictures high on the walls.
- **The hero corner's set**: pieces within the hero corner's footprint beside the parts the template places: speakers
  either side of the TV's cabinet, a media rack with LEDs beside it under the plaque, pads on the rack and on the rug
  with their cables to the console, a coffee table with magazines and an open box before the seat, a side table with
  a small lamp, a cushion on the seat; per pack (the 90s room a crate, the lounge a side table only, the study a
  joystick on its desk, the arcade none). The cabinet's top stays the CRT's and the console model's (ADR-0019 §7's
  42.5 x 40 cm), the CRT's line of sight from the doorway (style §6.1) and the passage to it stay as the plan has
  them. And one **proxy** of the game played last, standing in the console's slot (or lying on a model without one).
- **Tall decoration variants**: a decoration socket's variant may reach the wall's height (a bookcase of magazines
  and manuals, a record or tape rack, a vitrine of controllers, a tower speaker, a coat stand, a pegboard of pads,
  palms and ficus) within the socket's 0.9 x 0.4 m footprint and under the ceiling; heights alternate and no two alike
  stand side by side on a wall (style §13.4). A picture over a fixture is lifted by the drawn piece's height, not the
  plan prefab's.
- **Shelf detail**: alphabetical letter tabs on the spine-out levels (the level's first and last initials), a caption
  card under each face-out title of a display case (title; year and developer), and a section sign over the first
  module of each section of a shared room (the section's logo, else its name). They are atlases rasterized on the pool
  (as the plaque and the signs) on small plates, following live updates and the editor's moves.
- **Atmosphere and life** (§4, §6).

*Amended on 2026-11-08 (I12 step 1):* the shell and the look lights as built ([i12/step-1.md](../i12/step-1.md)):
- The **picture rail** runs at 2.70 m, above the doorway's casing (2.37 m) and so unbroken over it; the **wallpaper**
  covers the wall from the wainscot's top (1.0 m) up to the rail, where it shows between and above the modules
  (1.775 m tall), and the strip between the rail and the cornice keeps the room's plaster (a pack may name a
  **frieze** material for it; none does). The **skirting** stays the `room_shell`'s baseboard: a second board 18 mm
  proud would show only between the modules and cut into their gap to the wall. The cornice and the beams are as
  above (the beams every 3 m from the north wall while 1 m short of the south one).
- The **lamps** are a drum near the doorway, 1.5 m into the room on the doorway's axis, and a small spot can over
  each display case standing free of the walls, its lens 2.35 m up (its light is the profile's case light, already
  there): drums over the cases filled the doorway's view, where the concept has small warm spots. None hangs along
  the axis between the doorway and the hero wall, where a drum at 2.1 to 2.4 m hides the sign from the doorway in a
  grown room.
- **One look light per room**, the doorway drum's, drawn **after** the plan's lights **and the accents**, in a slot
  they leave free: drawn before the accents it took a display case's or the featured shelf's light in the rooms of
  890 and 2000 titles (an unlit case beside a lit one). A room whose plan lights and accents fill the 16 (most of the
  golden plans' rooms of 890 titles and more) shows its drum by its own glow. No light per growth step.
- The lounge's and the handhelds' **coves** run at 2.75 m, just above the rail.
- The seats' **woven fabric and grained leather** are new materials the packs' seats take (`MAT_*_Weave`,
  `MAT_Fabric_Leather_Grain`); `MAT_Fabric_Sofa` and the leathers stay plain, so the test room's armchair and the
  hub's seats keep their look (§7).

*Amended on 2026-11-09 (I12 step 3):* the hero corner's set and the last-played game as built
([i12/step-3.md](../i12/step-3.md)):
- The set is a pack's `furniture.heroDressing`: named pieces of six new prefab kinds (`floor_speaker`, `media_rack`,
  `pad`, `low_table`, `cushion`, `open_box`) and of existing ones (a small `floor_lamp` with an emissive shade and no
  light, `magazine_stack`, the 90s room's crate a `storage_box`), each placed in the hero corner socket's frame and
  belonging to one hero part (its **anchor**): drawn only where that part is (a room without a console has no pads),
  moved with the part when the plan's part stands elsewhere. Small pieces resting on others are **props**: they never
  collide and are not judged as fixtures; the others are, each as a part of the corner of its own
  (`<socket>.dress-<name>`, which the validator takes as the corner's).
- **Per pack:** classic and handhelds speakers, a rack, a coffee table with magazines and an open box, a side table
  with a lamp, a cushion on the seat (the handhelds without pads); the 90s collector a crate with magazines between
  its bean bags and a keep case on the rug; the lounge a side table with a lamp and a keep case beside its sofa, a
  cushion on it; the study a joystick on its desk; the arcade and the hub none.
- **The last-played game** is the room's placed game Retroverse started last (its play sessions, which the items
  files carry; ES-DE's `<lastplayed>` is not in the room's snapshot), standing in the console's media socket as Avvia's
  insertion leaves an item, a collection of its own drawn under `collection/lastplayed:<room>`. It is hidden whenever
  the walker is not roaming free (Focus, Inspect, the insertion, the editor) and while a launch runs; it follows at
  once a launch that played a game in its room, and the console's socket when the user's model arrives. A console
  without a media socket shows none (no "lying on the model" in this step); hovering it does nothing yet.

Rules of the layer:
- Nothing of it stands in a collection slot, in the circulation band, in the doorway's clear area or in the TV's line
  of sight; the main passage stays 1.4 m and the hard clear width 1.2 m (ADR-0010 §2). `validatePlan()` of the plan
  holding the drawn pieces (I6 step 3b's test, now with a pseudo-fixture per hero piece) finds no hard finding and the
  same soft findings as the plan alone.
- A piece's collider is inside the footprint it fills (a hero corner part's, a decoration socket's) or it has none
  (props lying on furniture, pictures, cornice, beams, pendants, tabs, cards, cones, dust). Pieces of the layer are
  never plan fixtures and are never picked as such by the editor; a decoration fixture the editor moves takes its
  variant and its picture along.
- A piece that would leave its footprint or rise through the ceiling, or that the pack's data cannot resolve, is not
  drawn and the plan's prefab is, logged once (I6 step 3b's guard, extended).
- **Readability** (style §26.2 point 9): no piece of the layer reads as a game: a bookcase's spines are plain cloth and
  paper from the pack's materials, never the label palette, with no fronts; an open box shows its tray and a manual,
  never a cover; the proxy is the only item of the layer that looks like a game, and it is one (its card opens on
  interact).
- **Proxies** are the hub showcase's kind (ADR-0014 §3): a collection of their own with a cover loader and a pick
  index of their own, never plan placements. The room's proxy is its placed game with the latest Retroverse session
  (the items file's `lastPlayed`), else none; it is hidden while a launch from its room runs, and remade from the
  catalog's bulk read when the launch returns and after a refresh.
- Stored worlds show the layer at once: it needs no update, no migration and no regeneration, because the plan is
  unchanged. The one exception is §3's density.

*Amended on 2026-11-10 (I12 step 5):* the shelf detail as built ([i12/step-5.md](../i12/step-5.md)):
- **One object a room** for its letter tabs and one for its caption cards (a prefab kind `label_plates`: each plate its
  own frame and atlas cell), each with one atlas of its room's texts (`gl::GpuMemoryKind::LabelTextures`), rather than
  one a fixture; a module moved makes its room's object again with the same atlas. They follow the room editor at the
  end of its session, as every runtime object does.
- **A section sign** stands over the first fixture of its section that holds its first item by sort key: a module, the
  wall shelf or a display case (a small shared room holds every title face-out), at a display case's back; its generic
  picture is the section's name, the identity's sign going over it as on the hall's signs. The arcade's cabinets hold
  none.

### 2. The gallery: the user's art on the walls

Every pack's `roomDressing` gains a **gallery** (the arcade's and the handhelds' `pictures`, extended to every pack): a
framed picture high on the wall over each shelf module (bottom at the rail, 2.25 m, top under the beams, 2.95 m), over
each low decoration piece and on the hero wall's sides. Their pictures are the user's media of the room's own games:

- **Kinds and frames.** Fanart as 16:9 posters, 3D boxes (else covers) as 4:5 portraits, marquees as 3:1 lightboxes
  (lit), screenshots, title screens and miximages as 4:3 frames with a mat. A picture is never stretched: it is fitted
  inside the frame's aspect on a mat of its own darkened dominant colour.
- **Choice.** The room's games ranked by the face-out priority (favourite, plays, rating, then the sort key), the
  frames walked in wall order, each taking a candidate of the kinds it accepts by `deriveSeed(roomSeed,
  "gallery-game", fixture)`; a game at most once per room. The pack weighs the kinds (the arcade lightboxes only, the
  lounges fanart first, the study screenshots first). A frame without a candidate shows the pack's era print.
- **Light.** Every other frame along a wall gets a picture-light hood with its wash (no real light, §1); lightboxes
  glow by themselves.
- **Reading.** The media's paths are derived as the covers' are (ADR-0008 §5) and carried by the rooms' items files,
  whose schema goes to 3 (the media paths, and the sort key, developer, publisher and genre that §1's shelf detail
  reads: one schema bump, one rewrite of the items files at the first start); decoded, fitted to the frame's size (at
  most 1024 pixels on the long side), mipmapped and BC1-compressed on the worker pool.
- **Residency.** The pictures of the whole resident floor stay resident under a GPU memory kind of their own
  (`GalleryTextures`), the eye's room's frames first and nearest first, released on a ride; a guard of 256 MiB evicts
  the farthest room first if a floor ever reaches it. A room whose pictures are not resident yet shows the prints.
- **Fixtures and goldens** show invented media (the fixtures' generated images); the real library's walls are checked
  by eye only, their screenshots under `build/`.
- **`world.gallery`** (world.json, settings.json, Impostazioni "Galleria della stanza": on or off): a display-only key,
  never in the digest; off, every frame shows the pack's print.

*Amended on 2026-11-08 (I12 step 1):* with the picture rail at 2.70 m (§1) the frames hang under it, as a rail's
pictures do: over a module a frame's bottom clears the module's top (1.775 m) and its top stays under the rail's
bottom (2.655 m), so the band is about 1.85 to 2.60 m and the tallest frame there about 0.75 m; the tall class over
low pieces keeps its limits under the rail too.

*Amended on 2026-11-09 (I12 step 2):* the gallery as built ([i12/step-2.md](../i12/step-2.md)):
- **Over the modules only** for now: one frame over each hero and side module against a wall. The low pieces' frames
  wait for step 4, which knows the drawn pieces' heights (a secondary TV or a tall variant stands over its plan
  prefab's top); the hero wall's sides keep their sign and plaque.
- **Sizes and band:** pictures of 704 x 396, 480 x 600, 780 x 260 and 600 x 450 mm inside 25 mm bars, each narrower
  than a module; hung between 80 mm over the module and 50 mm under the rail (and under a cove: the arcade's), in
  the band's middle, at most 350 mm over its lowest place.
- **Picture lights:** a hood on an arm and its wash on the wall (a new surface, a cone from the hood) over every other
  frame but the lightboxes.
- **Pictures:** made at 1024 x 576, 640 x 800, 1024 x 344 and 768 x 576 pixels, BC1 with their mips where the GPU
  takes S3TC; the mat the dominant colour of the medium's opaque pixels darkened (a lightbox's nearly black). The
  candidates are taken when the floor is built, from the items files (schema 3, which carries the caption cards'
  developer, publisher and genre too, filled now).
- **Residency:** the floor shown's pictures stay resident; the guard stops asking at 256 MiB of estimated pictures a
  scene (the frames past it keep their prints) rather than evicting the farthest room.
- **No overlay fixture:** `esde/snes-room` already has every kind of medium (its first game's fanart, marquee, 3D box,
  screenshot and miximage, a cover for nearly every game, a truncated screenshot).

### 3. The density default: 0.65

The curated preset's `decorationDensityPermille` becomes **650** (style §6.3's "ricca" band), so three of a room's four
dedicated decoration sockets are dressed where two were. The key shapes a plan (ADR-0018 §2), so:
- new worlds take 650;
- stored worlds keep what they were planned with until the user regenerates a room, a floor or the world through
  "Rigenera" (ADR-0015 §5, §6: the preview names the change);
- moving the default never marks a stored building as changed and never gives it a new validation finding: if the
  default reaches a stored plan's digest, its unchanged restart or the `decoration-density` rule, they read the plan's
  own recorded value instead;
- the golden plans keep 400 in their inputs, as the provenance of the generator's rules;
- the key stays out of `settings.json` and Impostazioni, as every plan-shaping key; a world whose `world.json` names a
  density keeps it.

*Amended on 2026-11-09 (I12 step 4):* the density and the tall pieces as built ([i12/step-4.md](../i12/step-4.md)):
- **The plan records each room's density** (`Room::decorationDensityPermille`, the room's key named only when it is
  not 400, so every earlier plan reads and writes as it was): a generation or a regeneration of a room lays it out
  with the configuration's density and records it; an update, a growth and the editor lay a stored room out with its
  own; the `decoration-density` rule judges each room by its own. A new room entering a stored building (a system
  imported since, an expansion) takes the configuration's. The inputDigest keeps the configuration's density as an
  input (it is provenance): after the preset's change an update rewrites a stored plan's digest once and moves
  nothing. `WorldConfig`'s own default stays the rules' 400.
- **The tall pieces** are four prefab kinds (bookcase, record rack, pegboard, coat stand), the plant's palm, ficus and
  fern forms, a vitrine of pads and a tower speaker, in every pack's decoration lists but the hub's. **The rhythm** is
  the scene builder's, once a room, along each wall in the sockets' home order: beside a neighbour within 1.2 m a draw
  of the same piece, or of the same class when both stand on the floor (tall from 1.5 m), is drawn again among the
  variants that differ. A hung piece takes no part in the alternation: counted tall, a poster pushed every piece beside
  it to a low one.
- **The gallery** hangs a frame over each low decoration standing against a wall, over its drawn top; none over a
  tall piece.
- **The look pendant's light** (§1, step 1) is a spot straight down: as a point 1.5 m inside the doorway it lit the
  hall's floor and wall beside the doorway through the portal's plane.

### 4. Atmosphere: dust, light cones, flicker, the screen's glow

- **Dust motes** drift in the key light's cone and in the pendants' pools: an instanced billboard pass after the
  blended batches, a few hundred quads per room, additive into the radiance (never the emission: dust does not bloom),
  depth-tested without writing depth. The emitters are derived once from the scene's lights at scene build; each
  mote's position is a function of the simulation time and a per-room seed in the vertex shader, so the CPU does no
  work per frame. Culled with the area and the frustum.
- **Light cones** under the spot lights of the packs that ask for them (the lounge, the arcade): additive cone meshes
  with a soft falloff whose brightness follows the light's, never above the bloom's threshold.
- **The neon's flicker**: a material term of the simulation time in the unlit shader behind a material flag; the tube
  dips for about a tenth of a second every six to ten seconds, never in the first two seconds of a run.
- **The screen's light** (style §4.3's "fake light with the average colour updated slowly"): the decoder thread
  computes each frame's average colour on a coarse grid; the room's `crt_glow` accent takes it, eased over about half
  a second, snapping at a clip's first frame so a frozen clip's view is constant. No new light.
- Under `reduceMotion` the motes stand still, the flicker stops at full brightness and the glow keeps its fixed
  colour; the cones stay. Under `--fixed-dt` every term is deterministic at a given frame; the simulation time is
  wrapped hourly for precision.
- In VR the pass runs per eye within ADR-0017 §9's rows; the fast profile halves the motes and drops the cones. The
  test room (`m0_testroom.json`) declares no atmosphere, so G01 and G21 draw none.

*Amended on 2026-11-11 (I12 step 6):* the atmosphere as built ([i12/step-6.md](../i12/step-6.md)): the packs' air is
data (`atmosphere`: dust, lightCones, coneStrength, neonFlicker, crtBreathing), written into each room's environment or
area; every room pack has dust, the lounge and the arcade the cones, the hub none. The motes scatter forwards
(Henyey-Greenstein), the cones end 5 cm above the floor instead of fading against the depth they are drawn over, the
flicker never comes before 4 s (not 2), the frame's average colour is taken on the main thread when it is uploaded
(not on the decoder thread), and only a clip's frames move the glow (a still keeps the light's own colour).

### 5. The sound of the room

- **Synthesized, never sampled.** Every sound is generated at startup from a deterministic recipe (mains hum with its
  harmonics and a slow beat, pink hiss, a neon buzz with a crackle, the arcade's murmur, a clock's tick, footsteps as
  shaped noise bursts per surface, a clack, a case's hinge, a click, a tick), GPU-free in `rv_audio`, unit-tested for
  length, peak, loop and determinism. The provenance registry gains nothing, because nothing is third-party.
- **Ambience** (the `ambience` bus): looping emitters chosen by the room's pack: the hero TV (hum, and hiss while it
  shows a picture), the neon sign (buzz), the room's centre (the arcade's murmur, the study's clock), spatialized with
  the clips' rules (ADR-0016 §9: gain by distance, pan, the -9 dB duck), a little lower while a clip is heard, silent
  outside the eye's area, cross-faded when the eye changes area.
- **Effects** (the `effects` bus): one-shots in a fixed pool of voices from the walker's steps (a cadence from the
  distance walked, the sound by the surface under the walker: parquet, rug, runner; lower in VR, none while teleporting
  or while the interaction moves the walker) and from the interaction (the pull-out's clack, the case on Inspect, the
  insertion's click, the menus' tick, which plays under the duck).
- **Settings**: `video.audio.ambience` and `video.audio.effects` (the buses' volumes `video.json` already reads) get
  their rows in Impostazioni and `settings.json`; `RV_AUDIO=null` and `--audio off` keep every automated run silent;
  the run report counts the sources, the steps and the effects.

*Amended on 2026-11-11 (I12 step 7):* the sound as built ([i12/step-7.md](../i12/step-7.md)): the packs' tone is data
(`sound`: hum, hiss, buzz, murmur, clock gains), written into each room's environment or area beside its air; the room
tone is heard whole within 2 m of its emitter, then as 2 m / d but never below a quarter in its room (the clips'
steeper rule left the hum silent from the doorway), and not by the clips' proximity; the menus' tick sounds on a press
of a navigation action while a menu is open; the interaction's sounds play at the eye; the duck applies per sound, so
the tick plays over it; sound ignores reduceMotion.

### 6. Life: micro-animations

- Pieces of the dressing layer may **animate**: a ceiling fan's blades and a lava lamp's wax (the 90s collector), a
  wall clock with the real time (the study), blinking LEDs on consoles, racks and power strips, static on a secondary
  TV between its clips (never on the hero screen, never in place of its standby picture).
- Rotations are vertex-shader terms of the simulation time behind an instance flag, so the depth pre-pass and the
  shadow follow; blinks and the wax's glow are §4's flicker term with other parameters; no CPU work per frame. The
  clock's hands are the one exception: transforms set once a minute (the second hand each frame) on a static face.
- Off under `reduceMotion` (the fan still, the LEDs lit). Frame-limited runs show the clock at 10:10 unless
  `--clock` names a time, so a golden with a clock is deterministic.

*Amended on 2026-11-11 (I12 step 8):* life as built ([i12/step-8.md](../i12/step-8.md)): the fan's blades and the
clock's hands are scene objects with a `motion` (a spin, or a clock's hand), turned on the CPU each frame about their
pivot (a few items per room; the passes follow without a shader term), not an instance flag; the fan and the clock are
dressing primitives (the lava lamp the one new prefab, on the 90s set's crate); the clock hangs over the featured
wall shelf and keeps the time under `reduceMotion` (its second hand at twelve); the racks' activity LEDs blink and the
wax breathes by §4's flicker in two more modes; no power strip and no console LEDs; a secondary's static lasts at
least 0.3 s and at most 1.5 s, and only after a clip that showed.

### 7. Goldens and proof

- Every golden whose view changes is regenerated on purpose at the end of the step that changes it: step 1 moves
  nearly all of them (the dressing in every room and hall seen; the woven `MAT_Fabric_Sofa` of the test room's
  armchair moves G01 and G21, regenerated by hand where their tests have no update mode; the leather of the hub's
  seats moves G11 and G14); steps 2 to 5 the rooms' (steps 3 and 4 in one round); step 6 once for the dust, G01 and
  G21 excepted. No new golden: the kinds of room are the ones Gate D has.

  *Amended on 2026-11-08 (I12 step 1):* step 1 leaves G01, G21, G11 and G14 as they were: the woven and grained
  fabrics are new materials of the packs' seats, `MAT_Fabric_Sofa` and the hub's leather stay plain (§1), and the
  hub's halls take no shell dressing.
- Every step has unit tests for its choosers (sites, seeds, rhythm, budgets, synthesis) and one GPU smoke case; Gate
  D's measures (median luma, top quarter, colour and accent shares) are recomputed on the fixture rooms and must move
  towards the concept panels' (the gallery's colour share, the shell's top quarter).
- The benchmark path, the VR benchmark path and the busiest real floor's memory row (two screens, the gallery and the
  atlases resident) are checked at the steps that add passes or textures.

### 8. The memory budget: 4 GiB

At this plan's review the user set the GPU memory budget at **4 GiB from now on**. The roadmap's GPU memory rows
become: all the GPU memory the app tracks (textures, render targets, buffers: a run report's
`gpuMemory.peakTotalBytes`, measured as before) at most **4 GiB (4096 MiB)**, on the reference machine's 8 GB RTX 2080
SUPER.

- **Why.** The 512 MiB row was set for I3's single room and held the building, the screens and the identity within
  34.5 MiB of its ceiling (477.5 MiB on the busiest floor with two screens). The rooms' density is worth more than that
  margin; the reference GPU has sixteen times the old row.
- **What changes.** The rows, the tests that assert them and `scripts/gate.ps1` read 4 GiB. The residency remedies the
  old margin forced (per-floor procedural surfaces, ADR-0016 §12; the console models' smallest mips outside the
  walker's rooms, ADR-0019 §9; LRU tiers of a few rooms for this iteration's pictures and atlases) are no longer needed
  for the row: the resident floor holds its gallery, atlases and surfaces whole.
- **What stays.** The row is measured and reported at every gate as before, with its breakdown by kind, so the figure
  stays known; one floor stays resident (ADR-0013); the derived-texture cache on disk keeps its own bound (a disk
  budget, not GPU memory); the cover arrays keep ADR-0011's sizes (doubling their resolution is offered in the BACKLOG,
  not chosen).
- **Later.** I8's Low profile (style §20.1: integrated graphics at 30 FPS) will name its own, lower budget when it is
  planned; 4 GiB is the Balanced profile's on the reference machine.

## Amendments

- **Roadmap, "Acceptable performance":** the GPU memory rows at 4 GiB (§8); I12's rows: the busiest floor with two
  screens and the dressing resident within 4 GiB, the atmosphere pass within the benchmark path's and VR's frame rows.
- **ADR-0010 §2:** decoration density 0.65 (§3 above); §10 unchanged: no stored plan moves.
- **ADR-0013, "Memory":** the busiest floor reads against 4 GiB (§8).
- **ADR-0014 §1:** the dressing layer (§1 above) is what a pack may draw as a look; `roomDressing` and `furniture`
  are every pack's. §4's light selection is unchanged: the look pendant's light is drawn after the plan's lights and
  the accents, within the 16 (§1 as amended by step 1).
- **ADR-0016 §4, §9 and §12:** the `effects` and `ambience` buses carry I12's synthesized sounds (§5 above); the
  `crt_glow` accent takes the picture's average colour (§4 above); §12's memory row reads 4 GiB and its remedy is no
  longer needed for the row (§8).
- **ADR-0017 §9:** unchanged; the atmosphere pass counts in the VR rows.
- **ADR-0018 §2:** `world.gallery`, `video.audio.ambience` and `video.audio.effects` are display-only keys of
  `settings.json`; `decorationDensityPermille` stays a `world.json` key.
- **ADR-0019 §1 and §9:** the user's game media join the logos and covers as runtime content (§2 above); §9's row
  reads 4 GiB and its remedy is no longer needed for the row (§8).
- **Style §6.3 and §25:** as listed above.
- **Roadmap:** I12 runs after I11's steps 0 to 8 and 11 to 14, beside I11's 9, 10 and 15 (which wait on the user's
  downloads), before I8.

## Consequences

**Gains**
- Rooms read as the concept's: full walls, a dressed ceiling, the user's own art and colour, a lived-in hero corner,
  a room that moves and sounds a little.
- No stored plan changes; nothing trademarked enters the repository.
- The memory row stops shaping the look: no residency tier is needed for this iteration's textures.

**Costs**
- Every golden is regenerated at least once (step 1), the rooms' up to four times over the iteration.
- About twenty new prefab kinds and four procedural surfaces, resident for the run where their floor uses them; the
  gallery's and the atlases' textures on the resident floor.
- A particle pass and an audio synthesizer to maintain; the VR rows checked again.
- A 4 GiB budget excludes the integrated GPUs style §20.1 names for the Low profile until I8 gives that profile its
  own budget.

**Deferred** (BACKLOG, "Parked at I12's planning")
- Compact rooms for small collections (a second template; a plan change).
- Reflections: a probe per room.
- The arcade's real marquees and flyers, the handheld stations' light.
- Cover arrays at twice the resolution (offered with the 4 GiB budget, not chosen).
