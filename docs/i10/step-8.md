# I10 step 8: Gate VR

Roadmap row: I10 step 8. The gate's page: [testing/gate-vr.md](../testing/gate-vr.md).

## What was built

- **`scripts/gate.ps1 -Gate VR`**: Gate D whole, run as a gate of its own under the focus watcher (its rows in
  `build/gate-d/`), with `-Clean`, `-RealLibrary`, `-Benchmarks` and the real library's options passed on; then the VR
  mode's rows in `build/gate-vr/`: golden G21 in Debug and Release from the GPU runs' verdicts, and the VR benchmark path
  in Release against ADR-0017 §9.
- **`docs/testing/gate-vr.md`**: what the gate checks, how to run it, the 30-minute session in the headset that decides
  it, item by item, and the known limits.

## Proof

- The script parses (PowerShell's parser), and its VR rows read the files the GPU runs and the benchmark write: G21's
  `comparison.txt` per preset, the benchmark's WARN line.
- Each step's runs: Release verifications of steps 3, 4 and 7 in a separate worktree (the unit suite, the golden and
  `[xr]` GPU tests, the desktop goldens' images identical to the step before, G21 identical, the VR benchmark).

## Left to the user

- Running `./scripts/gate.ps1 -Gate VR` on a quiet machine (Gate D takes about an hour and a half), and the session in
  the headset on Meta's runtime and on SteamVR, which declares I10 done.
