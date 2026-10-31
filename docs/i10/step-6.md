# I10 step 6: the game on the CRT

Roadmap row: I10 step 6. Decision: [ADR-0017](../decisions/ADR-0017-vr-through-openxr.md) §7 (amending ADR-0006's
hand-over in VR). Built on step 5's UI ([step-5.md](step-5.md)).

## What was built

- **The capture** (`rv_platform_win_capture`, `window_capture.hpp`): Windows.Graphics.Capture through C++/WinRT (the
  Windows SDK's headers), a D3D11 device of the default adapter, a free-threaded frame pool; each frame the newest
  captured frame is copied into a texture of the window's content size. No cursor, and no yellow border where the
  system allows it (Windows 11). Its own library beside `rv_platform_win`, so that retroverse-cli and rv_tests stay
  without D3D11 and WinRT; only `rv_app` links it. Every WinRT exception is caught inside it.
- **The interop** (`gl::DxInteropTexture`, `rv_render`): WGL_NV_DX_interop2 registers the D3D11 texture as a GL texture;
  each new frame is blitted (locked for the copy alone, its first row the window's top, as an image's) into a
  GL_SRGB8_ALPHA8 texture of OpenGL's own, which the CRT's material samples between copies with no lock held.
- **The emulator's window**: the process runner's window search keeps the window it found (`ProcessEvent::window`),
  the launch session the one showing (`LaunchSession::window`), the product UI the running launch's
  (`ProductUi::launchWindow`).
- **The game on the hero CRT** (`XrGame`, `ScreenService::showGameWindow`): in VR, while the launch's emulator shows a
  window, its picture is the hero CRT's, its aspect kept on the tube (the clips' rectangle), the picture tube's look on
  it; the screen's own pictures come back with the game's end. A capture that cannot start (an emulator in exclusive
  fullscreen, a system without the capture) is logged and recorded once, and the game plays on.
- **The launch keeps the headset** (ADR-0017 §7): in VR the host never idles for a launch; the launch is followed every
  frame (`ProductUi::onIdle` from the update), the scene renders on, the room's decoders stopped as for any launch.
  While the game runs Retroverse reads no input (the pad plays the game) and its UI panel stays out of the view; the
  return comes with the game's end.
- **The run report's `xr` section**: the captures started, the frames captured and shown, the window's size, why a
  capture failed.

## Proof

- **Unit tests**: the report's new fields (round trip); the launch session's window (the existing session tests with
  windows shown and gone).
- **GPU smoke test** (`tests/smoke/test_xr_room.cpp`): the snes-room fixture's room in the fake headset, a cartridge
  inspected with the ray and launched with Avvia into this build's fake emulator, which shows a small window in the
  background (never focusable, as every launch test's); its window is captured onto the hero CRT while the headset
  renders every frame with both eyes.

## By hand, in the headset

RetroArch with an isolated configuration under `build/` (docs/reference/safety.md), set to a borderless window
("Windowed Fullscreen Mode" on, exclusive fullscreen off) and not to pause in the background ("Pause Content When Not
Active" off):

```powershell
build/msvc-x64-release/bin/retroverse.exe --vr --data-dir build/rvdata --start-room room:snes
```

- Inspect a cartridge, press "Avvia" (the left menu button, or A on Avvia): the game shows on the room's big CRT;
  play it with the Xbox pad. Walk around, sit in the armchair: the CRT keeps playing.
- Quit the game in the emulator (RetroArch's own exit): the CRT goes back to its clips, the card comes back.

## Left for later

- A large screen in front of the user (ADR-0017 §7's option) and a command to stop watching without quitting the game.
- The "in game" profile's lower render scale: step 7, with the dynamic render scale.
- The D3D11 bridge as a second graphics binding (ADR-0017 §2): Meta's runtime takes OpenGL (step 0), so it waits for a
  runtime that refuses it; SteamVR's support is the user's trial.
