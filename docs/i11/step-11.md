# I11 step 11: the game's window followed

ADR-0020 §2, roadmap I11 step 11. In VR the CRT shows the window that really holds the game, through what real
emulators show besides it:
- a splash or an update dialog before it;
- a game list beside a separate render window;
- a console window;
- a window destroyed and made again.

Before this step, the first visible window of the tree was captured for the whole session.

## What was built

- **`launcher::chooseGameWindow()` and `WindowFollower`** (`rv/launcher/window_choice.hpp`, GPU- and Win32-free):
  - each visible window has a rank: preferred by a profile's rule, normal, lesser (a tool window or an owned dialog),
    last resort (under 200 x 150 or a console's), or excluded (skipped by a rule, cloaked, minimized);
  - within a rank the largest client area wins, then the one shown longest, then the lower handle;
  - the follower moves to a better window only once it has stayed better for 300 ms, and at once when the followed one
    goes.
- **`LaunchRequest::windowRules`:** the defaults now. Step 12's profiles add their rules.
- **`Win32ProcessRunner`** describes every visible top-level window of the tree as a candidate: class, title (read
  without a message, so a hung emulator never blocks the watcher), client size, tool and owned styles, DWM's cloaked
  state, minimized.
  - It reports `WindowShown` and `WindowsGone` as before, whenever any window shows or none does, so the session's
    waiting rules do not move.
  - A new event, **`WindowChanged`**, reports the followed window when it moves while the tree stays visible.
    `LaunchSession` takes it as its window.
- **`XrGame`:**
  - a new window restarts the capture;
  - a capture that failed, or whose window closed (`closed()` is now read), is tried again 2 s later, up to 3 times;
  - a window whose samples stay black for 5 s is reported once: a warning, and the run report's `captureBlack`. At most
    0.2% of a sample's pixels may be lit (`isBlackPicture()`).
  - The run report also gains `captureWindows`, `captureClosed` and `captureColor` (the last sample's mean colour).
- **The foreground** (I10 step 10) goes to the followed window, and is asked again every 500 ms for 10 s while another
  window holds it. As before, this never happens in hidden or background runs.
- **The fake emulator** gains:
  - `--window-mode splash | two-windows | recreate`: gray splash, main and status windows, smaller than the game's;
  - `--window-delay-ms`;
  - `--black`.

## Tests

- **Unit:**
  - `test_window_choice.cpp`: every rank, the order within a rank, the rules (case-insensitive, a rule with no text
    matching nothing), the follower through a splash, a game list beside a render window, a window made again, one
    minimized and restored, two windows taking turns that never make it flap, and every window gone;
  - `test_picture_bounds.cpp`: black with a border line, a video's black, the mean colour.
- **The Win32 runner against the fake, in the background:** the session's window ends on the game's after a splash,
  beside a main window, through a window made again, and when it shows late.
- **GPU** (`--vr --xr-runtime fake`): after a splash and beside a main window, the CRT's last picture is the game's
  purple, not the other windows' gray; an all-black window is reported. I10's three capture cases are unchanged.

## Departures

- **No "splash" rule** (ADR-0020 §2 first said: no caption and shown under 2 s). A borderless game window has no
  caption either. Since a splash is smaller than the game's window, the area decides, and the hysteresis keeps the
  choice steady. ADR-0020 §2 is rewritten to the ranks.
- **Tool windows are not excluded but ranked lower.** The fake emulator's background window, and some emulators' only
  window, are tool windows.
- **Black is reported after 5 s, not 3:** longer than a game's usual black loading screen. ADR-0020 §2 and §5 say so.
- **A window made again may keep its handle.** Windows reused the handle in the runner test's remake, so the session
  sees no change. The capture's `closed()` and the retry cover it in VR.
