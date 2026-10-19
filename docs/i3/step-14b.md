# I3 step 14b: the media viewer and Informazioni

Roadmap row: I3 step 14b (wave 5, on 14a; 13b, 15a and 16 run beside it in their own worktrees). Decisions:
[ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §5 (Vedi media: the images a game has, left and right
page, Back closes; Informazioni: the description and the other versions' titles and regions; pictures through
`rv-texture:`, RmlUi never loads the user's files) and §8 (the quick catalog's own Vedi media and Informazioni open the
same screens), [ADR-0005](../decisions/ADR-0005-product-ui.md) (Italian strings in the RML documents, dp sizes,
bundled fonts, no colour transitions).

Vedi media (the card's button, the pad's Y in Inspect, the quick catalog's button and Y) opens the **media viewer** over
everything: one page per image kind the game has, in the order cover, back cover, physical media, 3D box, screenshot,
title screen, marquee, fan art (mix images, videos, manuals and custom files are no pages; videos and manuals are I5's),
the kinds it lacks skipped. Left and right (D-pad, arrows, shoulders, triggers; a click on Precedente / Successiva) page
without wrapping, Home and End jump, Back (B, Esc, Chiudi) closes it. Each picture fits the stage without stretching
(enlarged at most four times). Until it comes the page says it is loading; a file that cannot be read or decoded shows
the page's fallback ("Immagine non leggibile"); a game without images says so. **Informazioni** (the card's button, X)
shows the title, platform and year, developer, publisher, genre, players, the description (line feeds kept) and the
other versions of the title the room's plan knows with their region tags; Up and Down scroll it, Back closes it. From
either screen the other one of the same game opens in its place (X in the viewer, Y in Informazioni). Closing returns
to where they were opened: Inspect with the card, or the quick catalog with its selection.

## What was built

| File | What it holds |
|---|---|
| `rv/ui/media_viewer_model.hpp`, `src/media_viewer_model.cpp` (`rv_ui_model`, step 0a's stubs) | `ui::media_viewer` (document, ids, data model, `rv-texture:media-viewer-picture`, `kKinds` in the viewer's order), `PictureState`, `MediaPage`, `MediaViewerModel` (pages from the kinds a game has, paging without wrap, Home, End, Back), `fitPicture()` |
| `rv/ui/game_info_model.hpp`, `src/game_info_model.cpp` (`rv_ui_model`, new) | `ui::game_info` (document, ids, data model, `kMaxVersions` 12), `OtherVersion`, `GameInfoContent`, `InfoStep`, `infoStepOf()`, `scrolledTop()` |
| `rv/ui/media_viewer_screen.hpp`, `src/media_viewer_screen.cpp` (`rv_ui`, step 0a's stubs) | `MediaViewerScreen`: data model `media_viewer` ("viewer"), shown over the other documents without taking RmlUi's focus, the picture sized in dp from the stage as laid out (`afterUpdate()`), the clicks (`page('previous'/'next'/'close')`), `pictureBox()`, `stageBox()` |
| `rv/ui/game_info_screen.hpp`, `src/game_info_screen.cpp` (`rv_ui`, step 0a's stubs) | `GameInfoScreen`: data model `game_info` ("info", "versions"), the body scrolled by `scroll(InfoStep)`, the clicks (`act('close'/'media')`) |
| `assets/ui/media_viewer.rml`, `.rcss`, `game_info.rml`, `.rcss` (new) | the two screens: every sentence, a label per kind, "n.d." for empty fields, the hints; nothing takes the focus (`focus: none`), compact theme |
| `rv/app/game_screens.hpp`, `src/game_screens.cpp` (`rv_app_catalog`, new) | `viewerPagesOf(GameDetails)` (the stored media's paths as `esde::roomMediaPaths` derives them, in the viewer's order), `viewerMediaRoot(library.json)`, `viewerPicture()` (read with `core::readFileShared`, decoded to RGBA, reduced to 2,048 px in linear light), `VersionGroups` (a plan's duplicate-title groups), `otherVersionOf()` (title and `readVersionTags` region), `gameInfoOf()` |
| `src/game_screens_panel.hpp`, `.cpp` (`rv_app`, new) | `GameScreensPanel`: the two screens, one at a time; a loader thread of its own (the media root resolved again at each opening, every page asked at once), the uploads on the main thread (RGBA8 holding the sRGB values as encoded, mips, at most two a frame, under `GpuMemoryKind::HeroTextures`), the page shown registered as `media-viewer-picture`, the textures deleted at the close; the run report's `screens` |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `GameScreenEventRecord`, `GameScreensRecord` (`RunReport::screens`, "screens": frames shown, pictures, failures, the picture's and the stage's boxes, events media, page, picture, info, scroll, closed) |
| tests | `tests/unit/ui/test_media_viewer.cpp`, `tests/unit/app/test_game_screens.cpp`, `tests/smoke/test_media_viewer.cpp`, `tests/input_scripts/media_viewer.json`, `media_viewer_corrupt.json` (new); `world_card.json` and `test_world_card.cpp` (Back after each screen, the screens' events checked) |

### Hooks in shared files (13b, 15a and 16 touch the app's world wiring and the quick catalog too)

- `src/app/src/product_ui.hpp`, `.cpp`: `#include "game_screens_panel.hpp"`; `Impl::screens`, `versionLookup`,
  `screensOpen()`, `openMedia()`, `openInfo()`, `switchScreen()`; created in `create()` after the card; the quick
  catalog's `events.media` / `events.info` (two lambdas in `openCatalog()`); `panelOpen()` also while a screen shows (so
  the input context is the UI's and Back's `PanelBack` level comes first: `panelBack()` closes the screen);
  `handleEvent()` forwards only mouse events while a screen shows; the notice, label and card hide under the screens
  (one condition each); `visible()`; commands routed to the screens after the game-session panel; `screens->update()`
  before the sync calls, `afterUpdate()` after the view's update; lockstep waits (`waitsForCatalog`, `onIdle`) also for
  the viewer's pictures; `finish()` writes `report.screens`. Public: `openGameMedia()`, `openGameInfo()`,
  `setVersionLookup()`, `gameScreenOpen()`.
- `src/app/include/rv/app/app_catalog.hpp`: `AppCatalog::gameDetails()` (default nothing; the ES-DE catalog's
  `CatalogDatabaseSource::detailsOf()`: `SqliteCatalogQuery::details` on the main thread's connection, like 14a's
  `entryOf()`).
- `src/app/src/world_session.hpp`, `.cpp`: member `m_versionGroups`, `otherVersionsOf()`; `showCard()` opens the screens
  on the result's `showMedia` / `showInfo` (four lines after 14a's `m_card.update`); `world_live_update.cpp`'s
  `keepShown()` one line (`m_versionGroups`).
- `src/app/src/retroverse_app.cpp`: `startWorld()` gives the product UI the room's version lookup (one block).
- `src/app/CMakeLists.txt`, `src/ui/CMakeLists.txt`: the three new sources.

## Decisions taken within the step

1. **The screens belong to the product UI, not to the world**: the same `GameScreensPanel` serves the card and the
   quick catalog, so "the same screens" holds by construction. While one shows, `ProductUi::panelOpen()` is true: the
   input context is the UI's (the world gets no actions, so Inspect, its card and the walker wait as they are) and
   Back's existing `PanelBack` level closes it before anything else (the game-session panel, when it shows over a
   screen, keeps its Back). No new `BackAction` and no new input context were needed.
2. **The viewer reads the files itself, on a thread of its own**: every page is asked when the viewer opens (at most
   eight files), the media root resolved again from library.json and ES-DE's settings each time (an unplugged drive is
   seen), so neither the quick catalog without a room nor the room's snapshot is needed. The pages come from the
   catalog's stored presence (what the last import found); a stored file that is gone or damaged shows the fallback.
   The cover page is the cover file, not the hero texture: the hero picture is fitted to the front label's aspect
   (banded or cropped), the viewer shows each image at its own aspect.
3. **The textures hold the sRGB values as encoded in RGBA8 (`ColorSpace::Linear`)**, as 14a's card view does through
   `glTextureView`: the UI composites sRGB-encoded values with `GL_FRAMEBUFFER_SRGB` off.
4. **The picture's size is set in dp from the stage as laid out** (`data-style-width` / `-height`): RmlUi caches an
   image source's dimensions at its first load, so one `rv-texture:` key cannot carry each page's own size.
5. **Nothing in the screens takes RmlUi's focus**: the application gives them the commands; the quick catalog's focus
   stays where it was under them, and keys never reach its search field while a screen shows.
6. **Other versions come from the room's plan** (ADR-0010's duplicate-title entries and their representative, both
   ways: a duplicate lists its representative and its siblings): Informazioni from the quick catalog lists them for the
   room's system; for other systems it says there are none known (a catalog-wide title-key query is left for when rooms
   of other systems exist, BACKLOG candidate).
7. **The quick catalog's own Informazioni still toggles its card's longer details** (the screen's `info()`, which 15a's
   card work may revisit): the toggle shows under the screen and is harmless; its event now opens Informazioni.

## Tests

- `tests/unit/ui/test_media_viewer.cpp` (`[ui][media_viewer]`): the pages in the viewer's order whatever the order
  given, the missing kinds and the unpaged ones skipped, no page for a game without images; paging without wrap, Home,
  End, Back; pictures shown and failed; the fit (no stretching, enlarged at most four times); Informazioni's steps and
  scrolling; both documents' ids, data bindings, the one `<img>` and its `rv-texture:` source, a label per kind, the
  click events, no `transition` and no `px`.
- `tests/unit/app/test_game_screens.cpp` (`[app][game_screens]`): the pages' paths as ES-DE names them (a nested game,
  a stored extension without its dot refused), the media root from library.json's override and ES-DE's default, a
  picture read and reduced, a truncated file (ParseError), text named `.png` (Unsupported), a missing file (NotFound);
  the version groups both ways; a version's region tag; Informazioni's content and its list kept to 12.
- `tests/smoke/test_media_viewer.cpp` (`gpu.media viewer: ...`, hidden, lockstep, snes-room imported):
  `media_viewer.json` from Amber Lantern's Focus pose: the viewer from the card with its 8 pages, paged right to the
  end and back, the 3D box's pixels equal to the fixture's file (mean difference 2.4 of 255, required under 12),
  fitted at the file's aspect and as high as the stage; Informazioni from the card in its place, Back to Inspect, then
  Focus; the quick catalog's search "cobalt", Informazioni listing the other release with its region, Vedi
  media of the same game in its place (its kinds only), Informazioni again, Back leaving the catalog open; every
  picture shown at its file's size. `media_viewer_corrupt.json`: Bramble Burrow Rally from the quick catalog (searched
  as "rally": "bramble" also matches the developer "Bramble Bits" of other games, and the catalog keeps its selection
  when it is in the new list), its truncated screenshot's page failed (its fallback), the two others shown. No run
  logged a refused UI file.
- `tests/smoke/test_world_card.cpp` / `world_card.json` (14a's): ui_back after each Vedi media and Informazioni (79, 97,
  109, 115) closes its screen; the screens' events checked; 14a's expectations unchanged.

## Results

Debug only, on the reference machine (RTX 2080 SUPER), `RV_LAUNCH_BACKGROUND` set, other worktrees building meanwhile
(the build ran at `-j 4`: more parallel jobs ran the compiler out of heap).

- Build `msvc-x64-debug` with 0 warnings.
- `rv_tests "[app],[ui]"`: 317 passed, 1 skipped (the hidden bench); the step's own `[media_viewer]` and
  `[game_screens]` cases among them.
- `rv_smoke_tests "[media_viewer]"`: both cases passed; `"[world_card],golden*"`: the world card case and golden G01
  passed.
- Screenshots (not committed, the fixture's invented titles), under `build/msvc-x64-debug/smoke/media_viewer/`:
  `screens/box.png` (the viewer from the card on Amber Lantern's 3D box, "Confezione 3D, 4 di 8", the picture centred at
  its aspect, the buttons and hints), `screens/info.png` (Informazioni of Amber Lantern: Lumen 16 Home System, 1990,
  Bramble Bits, Harborlight, Azione, 1, the description, "Nessun'altra versione"), `screens/catalog_info.png`
  (Informazioni from the quick catalog on Cobalt Compass with "Cobalt Compass - Europe" under Altre versioni),
  `corrupt/corrupt.png` ("Schermata di gioco, 3 di 3", "Immagine non leggibile", Successiva dimmed). A first look showed
  the quick catalog's text ghosting through the 95%-opaque panels and the stage's tint broken around the picture: the
  panels are opaque and the stage untinted now.
- The product UI's CPU section in the Debug smoke run with the screens: p50 1.6 ms (Release budget is the gate's).

## Open issues

- **Clicks** on the screens' buttons (Precedente, Successiva, Chiudi, Vedi media) are covered by the documents' tests
  and the screens' code only: no input script can click.
- A ui_back 6 frames after Back to Focus left the room in Focus (the item still returning, 13a/13b's timing), so the
  script opens the quick catalog from Focus.
- Other versions are known for the room's system only (decision 6).

## For the merge

- **Order**: the hooks above are small additions next to 14a's; 15a's "Mostra nella stanza" changes the quick catalog's
  card and its `QuickCatalogEvents` (this step only sets `events.media` and `events.info` in `ProductUi::openCatalog`);
  13b's lift and dimming leave `showCard()`'s four new lines after `m_card.update` untouched.
- **15a**: a launch while a screen shows cannot start (the screens take the input); `ProductUi::panelBack()` closes a
  screen first only while the game-session panel is hidden.
- **13b**: in Inspect the north button's `ui_media` opens the viewer through `InteractionResult::showMedia`; the
  dimmed room stays dimmed behind the screens (they are drawn by the UI over the post-processed frame).
- **tests/smoke/test_media_viewer.cpp** repeats helpers of `test_world_card.cpp` (`ImportedRoom`, `comparePicture`):
  fold them into a shared header with 14a's.
- **CLAUDE.md** (not edited by the step):
  - "Running the app": Vedi media (the card's button, Y; the quick catalog's button and Y) opens the media viewer: the
    game's images one page per kind (cover, back cover, physical media, 3D box, screenshot, title screen, marquee, fan
    art; the missing kinds skipped), left and right page, Home and End jump, Back closes; a file that cannot be read
    shows its fallback. Informazioni (the card's button, X) shows the description, the metadata and the other versions
    of the title the room's plan knows with their regions; Up and Down scroll; X and Y switch between the two.
  - `--run-report`: `screens` (`viewerFrames`, `infoFrames`, `pictures`, `failures`, `pictureBox`, `stageBox`,
    `events`: media, page, picture, info, scroll, closed, each with the game and `from` card or catalog).
  - GPU smoke tests: `gpu.media viewer: ...` (`tests/smoke/test_media_viewer.cpp`); input scripts `media_viewer.json`,
    `media_viewer_corrupt.json`.
  - "Layout and layering": `rv_ui_model`'s `media_viewer_model`, `game_info_model`; `rv_ui`'s `media_viewer_screen`,
    `game_info_screen`; `rv_app_catalog`'s `game_screens.hpp`; `rv_app`'s `game_screens_panel`.
- **docs/roadmap.md**: row 14b done.
