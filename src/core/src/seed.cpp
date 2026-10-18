#include <rv/core/seed.hpp>

#include <array>
#include <charconv>

#include <rv/core/sha256.hpp>

namespace rv::core {

std::string_view SeedPart::bytes(std::span<char> buffer) const noexcept {
    if (!m_isInteger) {
        return m_text;
    }
    char* const begin = buffer.data();
    char* at = begin;
    if (m_negative) {
        *at++ = '-';
    }
    // The buffer holds kMaxIntegerDigits characters: '-' and 19 digits, or 20 digits, never more.
    const auto written = std::to_chars(at, begin + buffer.size(), m_magnitude);
    return std::string_view(begin, static_cast<std::size_t>(written.ptr - begin));
}

std::uint64_t deriveSeed(std::span<const SeedPart> parts) noexcept {
    constexpr std::string_view kSeparator("\0", 1);
    Sha256 hash;
    hash.update(kSeedTupleTag);
    std::array<char, SeedPart::kMaxIntegerDigits> digits{};
    for (const SeedPart& part : parts) {
        hash.update(kSeparator);
        hash.update(part.bytes(digits));
    }
    const Sha256Digest digest = hash.finish();
    std::uint64_t seed = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        seed = (seed << 8) | digest[i];
    }
    return seed;
}

} // namespace rv::core
