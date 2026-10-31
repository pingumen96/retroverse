# Reference hardware

Performance targets (spec §18.1 NFR-01..04) and golden screenshots refer to this machine. Golden images are only
compared on it, with the driver version recorded next to each image.

| Component | Value |
|---|---|
| CPU | Intel Core i7-9700K @ 3.60 GHz (8 cores) |
| GPU (primary) | NVIDIA GeForce RTX 2080 SUPER, driver 32.0.16.2101 (621.01; until October 29, 2026 32.0.16.1742, 617.42) |
| GPU (integrated) | Intel UHD Graphics 630, driver 26.20.100.7642 (must not be selected; executables export `NvOptimusEnablement`) |
| RAM | 64 GB |
| OS | Windows 11 Home 10.0.29671 |
| Display target | 1920 x 1080, 60 Hz, VSync on |

Low-profile targets (style §20.1: 30 FPS on integrated graphics) are measured in iteration I8.

## The goldens re-baselined to driver 621.01 (October 30, 2026)

The driver went from 617.42 to 621.01. The goldens compare only on the GL_VERSION their JSON records, so the 15 still
recording 617.42 were skipped here (G15, G17 and G20 had been rendered under 621.01 in I5 step 6). Each was rendered
again from main at 6338e017 in `msvc-x64-debug` and compared with its committed PNG through
`retroverse-screenshot-diff` (6/255, at most 0.5% of the pixels): G01, G02, G02s, G06, G07, G08a, G08b, G10, G11,
G12, G13, G14, G16, G18 and G19 all identical, 0 pixels differing, the largest difference 0/255, every PNG the same
byte for byte. The room goldens' JSONs were rewritten with `RV_UPDATE_GOLDEN=1` (glVersion, renderedOn, gitCommit,
and the `assets/world` hash that I5 step 6's presets had moved), G01's gpu fields by hand (`test_golden.cpp` has no
update mode). Every golden, G01 to G20, then passed in Debug and Release with none skipped. The measurements below
were taken under 617.42 and stay as recorded.

**G01 under the memory margin's formats (October 30, 2026).** The memory margin (`docs/i5/memory-margin.md`) stores
the emitted light and the bloom in `GL_R11F_G11F_B10F`; the room goldens G02 to G20 were rendered again on purpose,
G01 was not. G01's PNG is still the one rendered with `GL_RGBA16F` there: the test room renders 73 pixels beyond 6/255
from it (at most 38/255, 0.004%, on the edges of the neon sign and the lamp), within its tolerance (0.5%), so it
passes as is. A later change that moves G01 on purpose rewrites it under the new formats.

## First populated room (specification §18.1; Gate B, October 19, 2026)

Measured in Release at 1920 x 1080 with `--no-vsync` on the benchmark path (`tests/input_scripts/bench_snes_room.json`,
1,230 frames), the driver 617.42, the Balanced profile (one shadowed key light, ambient occlusion, bloom, FXAA); the
user's desktop programs running ([gate-b.md](gate-b.md)):

| Room | Frame p50 / p99 | FPS at p99 | GPU per frame p50 / p99 | Collection arrays | All GPU memory (peak) |
|---|---|---|---|---|---|
| The real SNES room, 890 titles | 3.29 / 6.80 ms | 147 | 1.37 / 2.25 ms | 33.6 MiB (BC1) | 214.9 MiB |
| `snes-stress`, 2,000 titles | 3.06 / 4.06 ms (run 1); 3.40 / 19.16 ms (run 2, the machine busy) | 247 / 52 | 1.27 / 1.83 ms | 76.7 MiB (BC1) | 251.8 MiB |

Limits fixed from these figures (docs/roadmap.md, I3, "Budgets"): frame p99 16.7 ms (60 FPS, NFR-03) and p50 8 ms on
the benchmark path; the GPU passes at their median within shadow 0.4, depth 0.4, ambient occlusion 0.5, scene 2.0,
post-processing 0.6 and outline 0.1 ms, 5 ms in all; the collection arrays within 80 MiB at 2,048 layers and everything
the renderer allocates (textures, targets, buffers) within 256 MiB (its own accounting, `gpuMemory`, not the driver's).
The VRAM the driver reports beyond that (its own overhead, other programs) is not measured yet (I8).

## The building (Gate C, October 22, 2026)

Measured in Release on the real library's building (44 rooms on 9 floors, 19,985 placements; [gate-c.md](gate-c.md)),
the driver 617.42, the user's desktop programs running (Gate C's runs 3 and 4):

| Measure | Figure | Acceptable limit |
|---|---|---|
| The busiest floor (computer: 10 rooms, 5,314 items), all GPU memory at its peak, cold and warm | 407.3 MiB (collection arrays 210.9 MiB, BC1) | 512 MiB |
| The lobby shown from the stored plan, after the process started | 2.0 to 2.2 s (the quick catalog's first page 1.1 to 1.3 s) | 5 s (NFR-01) |
| A ride from the lobby to the busiest floor's largest room (1,646 items): the view back, cold / warm | 2.50 s / 1.80 to 1.95 s | 10 s / 3 s |
| The same room whole after the ride, cold / warm | 16.7 to 17.2 s / 1.55 to 1.70 s | 30 s / 3 s |
| The SNES room's benchmark path on its floor (890 titles), frame p50 / p99 | 3.34 to 3.37 / 4.04 to 4.36 ms (230 FPS at p99 at worst) | 16.7 / 33.3 ms |
