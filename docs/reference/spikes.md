# I1 spikes

Reference moved out of `CLAUDE.md` (2026-10-20); `CLAUDE.md` keeps the essentials and points here.

Measurement executables for the three integration risks of I1 (`spikes/`, reports in `docs/spikes/`, decisions in
ADR-0004 to ADR-0006). They run frame- or time-limited like the app; measure performance in Release.

- Video (`docs/spikes/i1-video.md`): `spike_video.exe --generate-fixtures build/spike_video/fixtures --seconds 10
  --hidden --report build/spike_video/report.json --screenshot build/screenshots/video.png --data-dir build/rvdata`
  plays two generated 1280x720 30 fps clips with burned-in frame counters. `--videos <file>...` plays files read in
  place (never written; the ES-DE media folder only read), `--error-cases` shows each damaged file falling back to a
  card naming its failure; also `--count`, `--max-height`, `--no-vsync`, `--upload-fences`, `--priority above-normal`,
  `--list-encoders`. `./spikes/video/run_measurements.ps1` runs the Release series (3 x 60 s, interleaved). GPU checks:
  `./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter spike/video`.
- Product UI (`docs/spikes/i1-ui.md`, `docs/i2/step-6.md`): `spike_ui.exe --smoke --frames 2271 --hidden --no-vsync
  --fixed-dt 0.016667 --data-dir build/rvdata --report build/spike_ui/report.json` drives the virtualized quick catalog
  over the test room and 100,000 generated games (fixed seed; the only game by "Studio Mezzanotte" sits at position
  50,000) with a scripted SDL virtual gamepad through the UI actions (`--scenario quick-catalog`, the default: 153
  checks; exit 3 when one fails), with `--screenshot-at <frame>:<png>` (repeatable), `--ui-scale`, `--high-contrast`
  and `--interactive` (a real pad or keyboard). `--scenario user-bindings --frames 80 --data-dir
  build/spike_ui/user_bindings` needs `spikes/ui/fixtures/user_bindings.json` copied to that directory's
  `config/bindings.json` (never into `build/rvdata`, which the other runs share: the fixture moves `ui_down` off the
  D-pad); `--input-script <json>` drives the screen with UI actions alone and `--expect-selection <position>` fails the
  run unless the selection ends there (`--frames 120 --input-script tests/input_scripts/quick_catalog_select.json
  --expect-selection 99998`). The documents in `assets/ui` reload when saved in developer builds. GPU checks
  `gpu.spike_ui.scenario`, `gpu.spike_ui.input_script` and `gpu.spike_ui.user_bindings`: `./scripts/test.ps1 -Preset
  msvc-x64-debug-gpu -Filter spike_ui`.
- Emulator return (`docs/spikes/i1-launch.md`): `spike_launch.exe --target fake|retroarch|notepad --strategy
  direct|tree|confirm --policy minimize|keep --repeat N --report <json> --data-dir build/...`;
  `./spikes/launch/measure.ps1` measures. RetroArch only ever runs with the isolated append configuration `spike_launch`
  writes under `build/` (its own config, saves and the ROM folder untouched, checked with before/after snapshots); never
  launch emulators whose data cannot be isolated (DuckStation). Keep visible runs short. The `desktop` tests run the
  fake emulator (`tests/fake_emulator`) in fullscreen: `./scripts/test.ps1 -Preset msvc-x64-debug-desktop`.


# I10 spike

- XR (`docs/spikes/i10-xr.md`, ADR-0017): `spike_xr.exe --seconds 30 --data-dir build/spike_xr --report
  build/spike_xr/report.json` opens an OpenXR session on the app's GL 4.5 context (`XR_KHR_opengl_enable`) and renders a
  ray-cast stereo room at the runtime's pace, the left eye in the window. It needs a headset (start Link first):
  `--list` reports what the runtime offers without a session. `--runtime-json <manifest>` measures another runtime
  without changing the active one; also `--refresh-rate`, `--render-scale`, `--space`, `--wait-system-s`,
  `--frames`, `--warmup-s`, `--max-missed`, `--no-mirror`, `--hidden`. No automated GPU case; `spike_xr_tests` (label
  `unit`) tests its GPU-free core.
