# I10 step 11: the game's picture without its black bars

Asked by the user on October 31, 2026: RetroArch's game on the CRT showed black bars above and below. Two fits nested:
RetroArch, in a borderless window as wide as the monitor (2560 x 1440), pillarboxes a 4:3 game in its middle
1920 x 1440, and the hero CRT fitted that whole 16:9 window into its 4:3 tube (letterboxed: the picture filled
`(0, 0.125) 1 x 0.75` of the screen). Decision: [ADR-0017](../decisions/ADR-0017-vr-through-openxr.md) §7. Built on
step 10 ([step-10.md](step-10.md)).

## What was built

- **The client area** (`platform_win::clientAreaInCapture`): Windows.Graphics.Capture takes a window with its frame;
  the CRT shows its client area alone (DWM's extended frame bounds against the client rectangle), so a windowed
  emulator's title bar and borders are left out too.
- **The black bars found** (`app::findPictureBounds`, GPU-free): four times a second the client area is read back at
  256 x 144 (`gl::PictureProbe`: a linear blit down, an asynchronous read into a pixel buffer behind a fence, no stall).
  A row or a column is a bar while at most 6% of its pixels is lit (an emulator's notice in a bar's corner leaves it
  one); bars are taken in pairs, the thinner of the two on both sides (emulators centre the picture); a part narrower or
  lower than half the window is none (a dark scene, a logo on black).
- **Kept steady** (`app::PictureBounds`): content coming into a bar widens the crop at once (an emulator's menu over the
  whole window); a crop that narrows is taken only once eight samples in a row (about 2 s) found it, so that a dark
  scene does not zoom the picture in.
- **On the CRT** (`app::croppedPictureRect`, `ScreenService::showGameWindow`): the crop fills the tube at its own aspect
  (a 4:3 game fills the 4:3 tube, a wider one is letterboxed), through the material's picture rectangle reaching beyond
  the screen where the crop leaves the window out.
- **The run report's `xr`**: `captureCrop` (the crop last shown, in the capture's texture coordinates),
  `captureCropSamples`, `captureCropChanges`; the log says each crop ("VR: the game's window shows on the CRT from ...").
- **The fake emulator** (`--pillarbox`, `--min-window-ms`): its colour in the window's middle 4:3 between black bars,
  shown at least so long.

## Proof

- **Unit tests** (`tests/unit/app/test_picture_bounds.cpp`): a pillarbox, a hairline, unequal letterbox bars, a notice in
  a bar, dark frames and a logo; the crop narrowing after its samples, kept through dark frames and a moment's dark
  scene, widened at once; the cropped rectangle on a 4:3 tube; the report's new fields (round trip).
- **GPU smoke tests** (`tests/smoke/test_xr_room.cpp`): the fake emulator pillarboxed in its 16:9 window, captured with
  its frame and title bar: the CRT shows its client area's middle 4:3; the launch case's window without bars shows its
  whole client area (16:9).

## By hand, in the headset

- A SNES game from RetroArch fills the CRT's tube; RetroArch's menu (if opened) widens it to the whole window at once,
  the game's bars come off again about 2 s after it closed.
- The log's "VR: the game's window shows on the CRT from ..." lines.

## Left for later

- Content whose own picture has black edges for long (a game letterboxed by itself) is cropped as bars are: the crop
  keeps its aspect, so the picture is shown larger, never stretched.
