# I11 step 14: watching from VR

ADR-0020 §6; roadmap I11 step 14. Until now a game launched in the headset took the CRT until it ended. Nothing showed
it where the room has no CRT (the lobby, a hall, the quick catalog's launch). Nobody told the user that the game played
on the PC's speakers. This step adds a way to stop watching the game and go back to it, a large screen, and the audio
notice.

## What was built

- **Stop watching** (`app::HoldCommand`, `LaunchFlow::stopWatching()`):
  - Hold the left controller's menu button (xr_bindings.json's Start) for one second while a game's session runs in VR.
  - The session panel's Playing question shows at once, in the stop-watching words: "Hai smesso di guardare il gioco".
    "Torna al gioco" is focused, and "Il gioco è terminato" is the other choice.
  - The question stays while the game keeps the foreground; the window's focus changes never take it back. The
    session's own questions (UserConfirmReturn, StillRunning) replace it.
  - One hold fires once. A hold that began before the game, or that already fired, counts only after a release.
- **While the user does not watch:**
  - The capture ends and the CRT gets its pictures back.
  - The screens' decoders play again (`ScreenService::launchEnded()`).
  - Retroverse reads the controllers again: the panel takes the ray and the buttons. The pad and the keyboard stay the
    game's, because SDL delivers them only to a focused window and the game keeps the foreground.
- **The two answers:**
  - **"Torna al gioco"** (`backToGame()`): the decoders stop as for a launch (`launchStarting()`) and the window is
    captured again. The app gives the window the foreground again; no question comes back by itself.
  - **"Il gioco è terminato"**: the session ends as `UserConfirmReturn` ends it, with the game left running if it runs.
- **The large screen** (`xr::gameScreenPanel()`, `XrGame::onLargeScreen()`):
  - A quad 2.4 m wide, at the picture's aspect, 2.8 m in front of where the head was when the session began, at the
    eyes' height and facing them.
  - It shows the captured window wherever no hero CRT takes the picture, and always when xr.json's new `gameScreen` is
    `"large"` (default `"crt"`).
  - It is drawn by the panel pass. `gl::Panel` gained a UV rectangle (the crop, top row first) and an opaque
    linear-picture mode: the capture's sRGB texture is encoded for the display.
- **The game's sound** (`platform_win::defaultAudioOutput()`, `sameAudioOutput()`):
  - At a session's start in VR, the headset's endpoint is compared with Windows' default render endpoint. The headset's
    comes from `XR_OCULUS_audio_device_guid`, enabled when the runtime offers it; the fake runtime reports one with
    `--xr-fake-audio-device`.
  - The comparison ignores case, and a bare `{guid}` matches the endpoint id that ends with it.
  - When the two differ, the launching panel says so: "Il suono del gioco esce dal dispositivo audio predefinito del PC,
    non dal visore…". The panel stands before the game for 8 s.
  - Retroverse never changes the default device. Nothing is said when either endpoint is unknown.
- **Screenshots** (F12, an input script's `screenshot`) are now taken while a game plays in VR: they show only
  Retroverse's view.
- **The run report:**
  - Each launch records `watchStops`, `watchResumes` and `audioElsewhere`.
  - The `xr` section records `largeScreenFrames`, `stoppedWatchingFrames`, `audioNoticeFrames` and `audioOutputKnown`.
  - `captureShownFrames` now counts the large screen's frames too.

## Tests

- **Unit:**
  - `test_xr_watch.cpp`: the hold's table (60 frames of 1/60 s fire, once per hold; unarmed and disarmed holds) and the
    endpoint comparison.
  - `test_launch_flow.cpp`: stop, back to the game, stop again, then the game's end; the game declared over; the
    session's own question replacing it; the audio flag.
  - The large screen's pose, xr.json's `gameScreen`, the input script's `leftMenu`, `--xr-fake-audio-device` and the
    panel document's new texts.
- **GPU** (`test_xr_room.cpp`, fake runtime, the fake emulator under the allow-list):
  - **The lobby:** a launch where no CRT is bound shows on the large screen on every captured frame. The screenshot
    shows the fake's purple picture, 16:9, before the lobby.
  - **Stop and resume:** the menu held frames 300 to 380, "Torna al gioco" at 470.
    - The launch records one stop and one resume.
    - Between 60 and 120 frames are stopped, the capture starts again, and the decoders stop twice.
    - The screenshots show the stop-watching panel in the room, then the room once watching again.
  - **xr.json "large" in the SNES room:** every captured frame is on the large screen. With a fake headset endpoint
    the launch records `audioElsewhere` and the notice stands before the game (when the machine has a default audio
    output).

## Departures

- **The question is on the session panel, not on the game's card.** The card exists only for a launch from the room.
  The session panel serves every launch (the lobby's, the quick catalog's) and already held "Torna al gioco". It stays
  until answered: the room is back behind it, and the panel holds the controllers' input.
- **The large screen is drawn over the scene**, like the UI's panel (the panel pass has no depth). Anything nearer
  than 2.8 m in front shows behind it. In the lobby and the halls the space in front is open; a depth-tested screen is
  in the BACKLOG.
- **`gameScreen` is not on the settings screen.** It stays in xr.json for now; Gate I11's headset session will tell
  whether it is worth a row.
- **The audio notice lasts 8 s.** ADR-0020 sets no duration. The panel says it for the whole launch phase; only its
  place before the game is limited.
