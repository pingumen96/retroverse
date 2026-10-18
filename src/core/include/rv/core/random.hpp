#pragma once

// The world generator's random sequences (ADR-0010 §7): SplitMix64 with distributions of its own, defined here bit for
// bit, because the standard library's distributions (std::uniform_int_distribution, std::shuffle) are
// implementation-defined and a plan must be byte-identical across builds. Golden sequences come from the independent
// oracle `scripts/world_seed_oracle.py`. Never std::rand, never std::random_device: a sequence starts from a seed of
// core::deriveSeed().

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace rv::core {

/// The algorithm and its distributions as one versioned id, recorded in a plan (`randomAlgorithm`): any change of
/// next(), below(), between(), unit() or shuffle() is a new id.
inline constexpr std::string_view kRandomAlgorithm = "splitmix64/1";

/// SplitMix64 (Steele, Lea and Flood 2014; Vigna's splitmix64.c): a 64-bit state advanced by 0x9E3779B97F4A7C15, each
/// output the advanced state through a fixed mix. Every seed, zero included, gives a full-period sequence. Not for
/// cryptography; cheap to copy (one integer).
class Random {
public:
    explicit Random(std::uint64_t seed) noexcept : m_state(seed) {}

    /// The next 64-bit output.
    std::uint64_t next() noexcept;

    /// Uniform in [0, bound): outputs x are drawn until x >= (2^64 - bound) mod bound (so that every result is equally
    /// likely), the result is x mod bound. At least one output is drawn, also for a bound of 1. A bound of 0 gives 0
    /// and draws nothing.
    std::uint64_t below(std::uint64_t bound) noexcept;

    /// Uniform in [low, high], both included: low + below(high - low + 1) in 64-bit two's complement; for the whole
    /// range of std::int64_t, low + next(). `low` when high < low, nothing drawn.
    std::int64_t between(std::int64_t low, std::int64_t high) noexcept;

    /// Uniform in [0, 1): the output's top 53 bits times 2^-53, one output (exact in IEEE double arithmetic).
    double unit() noexcept;

    /// Fisher-Yates from the last item: for i = size - 1 down to 1, items i and below(i + 1) swapped (size - 1 outputs
    /// at least; nothing drawn for 0 or 1 items).
    template <class T>
    void shuffle(std::span<T> items) noexcept(std::is_nothrow_swappable_v<T>) {
        for (std::size_t i = items.size(); i > 1; --i) {
            const auto j = static_cast<std::size_t>(below(static_cast<std::uint64_t>(i)));
            using std::swap;
            swap(items[i - 1], items[j]);
        }
    }

    /// The state: the seed advanced once per output drawn.
    [[nodiscard]] std::uint64_t state() const noexcept { return m_state; }

private:
    std::uint64_t m_state;
};

} // namespace rv::core
