# ADR-0007: The builds use the vcpkg clone that VCPKG_ROOT names

- Status: accepted
- Date: 2026-10-16

## Context

ADR-0001 pins the vcpkg baseline and makes baseline changes deliberate, because they invalidate the binary cache. The
presets take the toolchain from `$env{VCPKG_ROOT}`, which the README and `scripts/dev.ps1` require to name a local vcpkg
clone: on the reference machine `E:/Progetti/vcpkg`, at the baseline `44ccfd88`, with the tool bootstrapped at that
commit (2026-09-26). The clone is shared with other projects of the same user.

The review at the merge of the I1 spikes found that the builds never used it. `scripts/dev.ps1` enters the Visual Studio
developer environment with `Enter-VsDevShell`, whose vcpkg extension (`Common7/Tools/vsdevcmd/ext/vcpkg.bat`, which
calls `VC/vcpkg/vcpkg-init.cmd`) sets `VCPKG_ROOT` to Visual Studio's bundled vcpkg (`VC/vcpkg`: read-only, tool
2026-07-27, its registry fetched from git) whatever the variable held; `dev.ps1` checked the variable only afterwards.
Every tree had been configured with `E:/Programmi/VS2026/VC/vcpkg/scripts/buildsystems/vcpkg.cmake`. The two copies
differ in the tool and in 16 of the helper scripts they share, and those scripts enter the packages' ABI hashes.

The options were the clone as documented; a clone for Retroverse alone (isolated from the other projects, about 2 GB
more); or the bundled copy, with which a Visual Studio update can rebuild every package at the next configure and a
later baseline may need a newer tool than Visual Studio ships.

## Decision

The builds use the clone that `VCPKG_ROOT` names, as ADR-0001 and the README meant (the project owner's choice).
`scripts/dev.ps1` reads `VCPKG_ROOT` before it enters the developer environment and restores it afterwards. Inside a
Visual Studio developer shell, whose process already holds the bundled copy, it takes the user's or the machine's own
setting. It refuses to run when neither names a clone (a folder with `vcpkg-bundle.json` is Visual Studio's copy).

## Consequences

- The switch rebuilt the packages once: every tree was reconfigured from scratch on the clone (`./scripts/build.ps1
  -Preset <preset> -Fresh`; each tree caches its toolchain file, so an incremental configure keeps the old one). On the
  reference machine the first tree took 24 minutes (54 packages, FFmpeg alone 18 minutes), the other two 5 minutes each
  (the project itself); Gate A then passed unchanged (697 unit tests per configuration, golden G01 identical).
- The packages are rebuilt only when the clone changes: a baseline update (ADR-0001), or an update of the shared clone
  made for another project, which changes the tool or the helper scripts. Visual Studio updates no longer do.
- vcpkg works in the clone's `buildtrees`, `packages` and `downloads` and holds the clone's lock while it installs,
  alongside the other projects that use the clone.
- A machine without a clone makes one at the baseline and bootstraps it (README).
