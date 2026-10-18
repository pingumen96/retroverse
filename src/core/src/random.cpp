#include <rv/core/random.hpp>

namespace rv::core {

std::uint64_t Random::next() noexcept {
    m_state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = m_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::uint64_t Random::below(std::uint64_t bound) noexcept {
    if (bound == 0) {
        return 0;
    }
    // 2^64 mod bound outputs at the bottom are rejected: what is left is a whole number of copies of [0, bound).
    const std::uint64_t limit = (std::uint64_t{0} - bound) % bound;
    while (true) {
        const std::uint64_t x = next();
        if (x >= limit) {
            return x % bound;
        }
    }
}

std::int64_t Random::between(std::int64_t low, std::int64_t high) noexcept {
    if (high < low) {
        return low;
    }
    // The span in unsigned arithmetic, where it cannot overflow; 0 means the whole 2^64 values.
    const std::uint64_t span = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low) + 1u;
    const std::uint64_t offset = span == 0 ? next() : below(span);
    // Two's complement (guaranteed by C++20): the conversion of the sum back is exact.
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + offset);
}

double Random::unit() noexcept {
    return static_cast<double>(next() >> 11) * 0x1.0p-53;
}

} // namespace rv::core
