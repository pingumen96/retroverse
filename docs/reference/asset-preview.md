# Asset preview

Reference moved out of `CLAUDE.md` (2026-10-20); `CLAUDE.md` keeps the essentials and points here.

`retroverse-asset-preview.exe` (style §19.3, `tools/asset-preview`; developer builds only, it needs the Dear ImGui
overlay) shows one asset in the model viewer's neutral studio with the host, renderer, scene instantiation and overlay
of `rv_app` (`app::SceneGraphics`, `app::SceneInstance`, `devui::DevUi`): `retroverse-asset-preview <file.glb |
file.gltf | manifest.json | prefab:<kind>>` with `--params '<json object>'` for a prefab, `--view lit | base-color |
normals | uv-checker | wireframe`, `--panel <title>` (a tab brought forward at start, for screenshots) and the host
options of `retroverse.exe` (`--smoke`, `--exit-after-frames`, `--frames`, `--hidden`, `--screenshot`,
`--screenshot-size`, `--data-dir`, `--fixed-dt`, ...); without a target it shows the empty studio and waits for a file
dropped on the window. It logs to `<user data>/logs/asset-preview.log` and keeps its layout in
`<user data>/config/asset-preview-imgui.ini`. `--write-fixture <file.glb>` writes a CRT-like test model (Body,
ScreenSurface, Anchor_TV_Stand, Anchor_ScreenLook, ColliderProxy; 71 x 61 x 56 cm) and exits without a window.

- Controls: left drag orbits, middle or right drag pans, the wheel zooms (gamepad: right stick, left stick, triggers); F
  frames the subject in the part of the view the panels leave visible (the framing follows the panels until the camera
  is moved), and pressed again before the camera moves frames it with its sockets and scale references (the empty
  studio frames those); R toggles the auto-rotation, 1 to 5 pick the view mode; F1 hides the panels (the labels in the
  scene stay), F5 reloads the shaders, F12 writes a screenshot; Esc closes a popup, else hides the panels, else quits.
  The subject's files are polled: a change reloads it, also one of a .gltf's buffer or image files and a manifest
  created next to a model; the procedural surfaces stay generated across reloads.
- Overlays (Asset panel): floor grid just below y = 0, so that a floor there hides it (10 cm lines on subjects up to 6 m
  across, brighter every meter), a 1 m cube and a 1.70 m marker left of the subject, the bounds with W/H/D in cm, node
  bounds, pivot axes, socket axes with their names, slot volumes (green) and level volumes (teal) with the sockets,
  ScreenSurface outlines, collision proxy outlines, vertex normals.
  The labels are drawn by the overlay, kept clear of each other and of the labelled points: in screenshots they line
  up with the scene when `--screenshot-size` has the window's aspect (1920x1080 for the default 1280x720 window). The
  inspection views (base color, normals, uv checker) show their values exactly (no tonemap, no FXAA) on the lit view's
  background.
- Panels: Asset, Model (node tree with each primitive's triangles, vertices and material, or a prefab's parts and
  sockets), Statistics (bounds in cm, primitives as potential draw calls, triangles, vertices, textures with estimated
  GPU memory, glTF extensions, the slots by volume: "2.0 x 12.5 x 8.5 cm: 231 in 7 levels"), Materials (factors and
  texture thumbnails; glTF materials listed by their own name, then the model file), Manifest (its fields and the
  `validatePrefabManifest` results; brought forward when a manifest comes with a new target or appears next to the model
  as `<name>.manifest.json` or `<name>.json`, not on reloads nor over `--panel`), Screen (each ScreenSurface with its
  size as placed, node scale included, and the color bars it shows: glTF screens are drawn with `MAT_Screen_CRT` and the
  pattern instead of their own material), Stats, Warnings, Log. Screenshots include the panels from the third frame on
  (`--frames 3` or more; an earlier `--screenshot` logs a warning).

```powershell
build/msvc-x64-debug/bin/retroverse-asset-preview.exe --write-fixture build/preview/crt_fixture.glb
build/msvc-x64-debug/bin/retroverse-asset-preview.exe build/preview/crt_fixture.glb --exit-after-frames --frames 5 `
  --hidden --screenshot build/screenshots/preview_fixture.png --data-dir build/rvdata
build/msvc-x64-debug/bin/retroverse-asset-preview.exe assets/manifests/prefabs/electronics.crt.consumer.generic.json `
  --exit-after-frames --frames 5 --hidden --screenshot build/screenshots/preview_manifest.png --data-dir build/rvdata
```

The fixture shows W 71.0 / H 61.0 / D 56.0 cm, three socket names (Anchor_ScreenLook, 0.9 m in front of the screen,
lies outside the default framing; a second F shows it) and the color bars on its screen; the CRT manifest builds
`prefab:crt` and reports one warning (the placeholder hash), and with `--panel Materials` shows that tab instead of the
Manifest one; a copy under `build/` whose `runtime.sockets.screen` names a missing socket reports that error.

