# Reference hardware

Performance targets (spec §18.1 NFR-01..04) and golden screenshots refer to this machine. Golden images are only
compared on it, with the driver version recorded next to each image.

| Component | Value |
|---|---|
| CPU | Intel Core i7-9700K @ 3.60 GHz (8 cores) |
| GPU (primary) | NVIDIA GeForce RTX 2080 SUPER, driver 32.0.16.1742 |
| GPU (integrated) | Intel UHD Graphics 630, driver 26.20.100.7642 (must not be selected; executables export `NvOptimusEnablement`) |
| RAM | 64 GB |
| OS | Windows 11 Home 10.0.29671 |
| Display target | 1920 x 1080, 60 Hz, VSync on |

Low-profile targets (style §20.1: 30 FPS on integrated graphics) are measured in iteration I8.

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
