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
