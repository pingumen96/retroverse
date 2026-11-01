# Gate VR: a 30-minute session in the headset

Gate VR closes iteration I10, VR through OpenXR ([ADR-0017](../decisions/ADR-0017-vr-through-openxr.md)): a Meta Quest
(or any OpenXR headset) on the PC through Link, Air Link or SteamVR, the building walked with the stick, snap turns and
the teleport, used with the Touch controllers or the pad, the UI on a panel in the world, a launched game played on the
room's CRT. What each step built is in [docs/i10/](../i10/). This page says what the gate checks, how to run it, and the
session in the headset that decides it.

**Status: the user declared I10 done on November 1, 2026.** This page records no whole run of `gate.ps1 -Gate VR` and no
session on SteamVR (not installed here); the VR cases, G21 and the VR benchmarks passed at I10's merge into main.

## What the gate checks

`scripts/gate.ps1 -Gate VR` runs Gate D whole as a gate of its own ([gate-d.md](gate-d.md): the build matrix without a
warning, the unit tests and the GPU checks of Debug and Release, every golden, the benchmark path in Release, all under
`scripts/focus_watch.py`), with `-RealLibrary` and the real library's options passed on. Its GPU runs hold the VR mode's
cases with the fake runtime (`[xr]`: `tests/smoke/test_xr.cpp`, `test_xr_room.cpp`): the session's pacing, a lost
session and a missing runtime, the walk, the snap turn and the teleport, the seated posture, a controller's ray
inspecting a cartridge held in that hand, the quick catalog clicked on its panel, a launched game captured onto the hero
CRT while the controllers walk. Then it adds:

- **Golden G21** (both eyes in the test room, `assets/style_tests/screenshots/g21_xr_testroom_golden.png`) in Debug and
  Release, read from the GPU runs' verdicts; compared on the golden's GPU and driver only, like the others.
- **The VR benchmark path** in Release (`tests/smoke/test_world_benchmark.cpp`, `[.bench]`, run by this gate alone:
  the others select `[benchmark]~[xr]`): the snes-stress room of
  2,000 titles along `tests/input_scripts/bench_snes_room.json`, both eyes at 2048 x 2048 (the fake runtime at render
  scale 2, about Link's 2080 x 2096 on a Quest 3S), render scale 1, against ADR-0017 §9: frame p50 at most 13.9 ms and
  p99 at most 20 ms (72 Hz). Both eyes' GPU time is printed (twice the last eye's passes). The same path at 120 Hz
  with the fast profile (I10 step 9): p50 at most 8.3 ms, p99 at most 11.1 ms.

```powershell
./scripts/gate.ps1 -Gate VR
./scripts/gate.ps1 -Gate VR -RealLibrary -LegacyCli <generator 1.3's retroverse-cli.exe>   # ES-DE closed
```

The rows go into `build/gate-vr/`, Gate D's into `build/gate-d/`. The fake runtime paces nothing, so the missed
refreshes of §9 are measured in the headset only (the run report's `xr.missedRefreshes`).

## The session in the headset

Meta Horizon Link running, the headset in Link over the cable (the limits are the cable's; Air Link adds latency the
app does not control), then the same on SteamVR. A data directory with the user's library imported
(`retroverse-cli library create ...` and `import`); RetroArch with an isolated configuration under `build/`, borderless,
"Pause Content When Not Active" off.

```powershell
build/msvc-x64-release/bin/retroverse.exe --vr --data-dir <data directory>
```

For 30 minutes, each item checked:

1. **The lobby:** the room at its true scale, the floor under the feet, no swim when turning or leaning, both eyes
   agree; the mirror in the window.
2. **Moving:** the left stick along the head, snap turns of 30 degrees, the teleport aimed with the right stick (cyan
   ring, amber where it cannot land); leaning into a wall fades and pushes back; `posture: "seated"` in xr.json, then
   the headset's recentering.
3. **The elevator and the directory:** E or A facing the elevator, the left stick's click anywhere; the panel in front,
   a floor and a room clicked with the ray; the ride's fade.
4. **A room:** a cover pointed at (the laser cyan, the controller ticks), its title over it; the trigger inspects it in
   the hand, the wrist turns it; the card in the heads-up display; B puts it back; a spine-out item pulled out in place.
5. **The quick catalog:** Y, the ray on a game, the trigger selects, the right stick scrolls, "Avvia".
6. **The game on the CRT:** a SNES game launched from the card (the cartridge back on its shelf), played with the Xbox
   pad on the room's big CRT; a Touch controller picked up walks up to it meanwhile (I10 step 10); quitting it in
   RetroArch brings the room back.
7. **Comfort and pacing:** 72 Hz without stutter in a populated room (the run report's `xr` section: missed refreshes
   at most 1%, the dynamic scale's lowest), 90 Hz with `refreshRate: 90` in xr.json, 120 Hz with
   `--xr-refresh-rate 120` once the Meta app allows it.

The user declares I10 done after the session on Meta's runtime and on SteamVR.

## Known limits

- The UI is drawn into the eyes, not as compositor layers (ADR-0017 §6's fallback); the card stands in the heads-up
  display rather than beside its item (BACKLOG, I10's follow-ups).
- While a game runs in VR Retroverse reads only the controllers' walk, turns and teleport: the return comes with the
  game's end.
- The D3D11 bridge (ADR-0017 §2's second binding) is not built: Meta's runtime takes OpenGL (step 0); if SteamVR
  refuses it, the session falls back to the desktop and says why.
