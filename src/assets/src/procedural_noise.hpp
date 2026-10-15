#pragma once

// Internal helpers of the procedural surfaces (surface_*.cpp): deterministic integer hashing and tileable value noise.
// Not installed, not part of the API. Every function gives the same result on every platform and in every build: the
// lattice values come from integer hashes, interpolation uses plain float arithmetic.

#include <cstdint>

namespace rv::assets::detail {

/// Deterministic 32-bit hash of the lattice point (x, y) under `seed`.
[[nodiscard]] std::uint32_t hashPoint(std::int32_t x, std::int32_t y, std::uint32_t seed) noexcept;

/// hashPoint as a float in [0, 1).
[[nodiscard]] float hashUnit(std::int32_t x, std::int32_t y, std::uint32_t seed) noexcept;

/// Seed of a numbered sub-pattern (octave, plank, board) derived from `seed`.
[[nodiscard]] std::uint32_t subSeed(std::uint32_t seed, std::uint32_t index) noexcept;

/// floor(x) as an integer, for |x| below 2^31.
[[nodiscard]] std::int32_t floorToInt(float x) noexcept;

/// x - floor(x), in [0, 1).
[[nodiscard]] float fract(float x) noexcept;

/// Hermite smoothstep of x between edge0 and edge1 (edge0 != edge1), clamped to [0, 1].
[[nodiscard]] float smoothStep(float edge0, float edge1, float x) noexcept;

/// Value noise in [0, 1] at (x, y) in lattice cells, with C2-continuous (quintic) interpolation. The lattice repeats
/// every `periodX` cells along x and `periodY` cells along y (each >= 1), so noise sampled at (u * periodX,
/// v * periodY) for u, v in [0, 1) tiles seamlessly; large periods give noise that never visibly repeats.
[[nodiscard]] float valueNoise(float x, float y, std::int32_t periodX, std::int32_t periodY,
                               std::uint32_t seed) noexcept;

/// Tileable fractal noise, roughly in [-1, 1] (mean 0): `octaves` value noises over the unit square, octave i with
/// (cellsX, cellsY) * 2^i cells (each octave repeats over the square) and gain^i the weight, normalized by the sum of
/// the weights. Sample it at u, v in [0, 1) (values outside wrap around).
[[nodiscard]] float fbm(float u, float v, std::int32_t cellsX, std::int32_t cellsY, int octaves, float gain,
                        std::uint32_t seed) noexcept;

} // namespace rv::assets::detail
