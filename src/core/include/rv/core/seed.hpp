#pragma once

// Seeds of the world generator (ADR-0010 §7): a 64-bit seed derived from a tuple of parts, so that every decision draws
// from a random sequence of its own and one decision never shifts another's. Golden seeds come from the independent
// oracle `scripts/world_seed_oracle.py` (Python's hashlib), as the GameIds' do.

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace rv::core {

/// The first element of every seed's tuple: a change of the derivation is a new tag.
inline constexpr std::string_view kSeedTupleTag = "rv-seed/1";

/// An integer type that a seed part takes as a number: bool and the character types (char, wchar_t, char8_t, char16_t,
/// char32_t) are left out, so that a stray 'a' or `true` never becomes "97" or "1"; signed and unsigned char are the
/// 8-bit integers (std::int8_t, std::uint8_t) and count as numbers.
template <class T>
concept SeedInteger =
    std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool> && !std::same_as<std::remove_cv_t<T>, char> &&
    !std::same_as<std::remove_cv_t<T>, wchar_t> && !std::same_as<std::remove_cv_t<T>, char8_t> &&
    !std::same_as<std::remove_cv_t<T>, char16_t> && !std::same_as<std::remove_cv_t<T>, char32_t>;

/// One element of a seed's tuple: text as its UTF-8 bytes, an integer as its decimal ASCII digits ('-' before a
/// negative one), so that deriveSeed(7) and deriveSeed("7") are one seed (a parent seed enters its children's tuples as
/// a number). Text is viewed, not copied: a part lives for the call it is passed to. Text parts are identifiers (ids,
/// GameIds, family names), validated where they are read: a NUL inside a part would make two tuples one, and is hashed
/// as given.
class SeedPart {
public:
    SeedPart(std::string_view text) noexcept : m_text(text) {}
    SeedPart(const char* text) noexcept : m_text(text) {}
    SeedPart(const std::string& text) noexcept : m_text(text) {}
    template <SeedInteger T>
    SeedPart(T value) noexcept : m_isInteger(true) {
        if constexpr (std::is_signed_v<T>) {
            m_negative = value < 0;
            // The magnitude in unsigned arithmetic: -(minimum) does not overflow there.
            m_magnitude =
                m_negative ? std::uint64_t{0} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
        } else {
            m_magnitude = static_cast<std::uint64_t>(value);
        }
    }

    /// The part's bytes as they enter the tuple: the text, or the integer's digits written into `buffer` (at least
    /// kMaxIntegerDigits characters).
    [[nodiscard]] std::string_view bytes(std::span<char> buffer) const noexcept;

    /// The longest integer part: '-' and the 19 digits of 2^63, or the 20 digits of 2^64 - 1.
    static constexpr std::size_t kMaxIntegerDigits = 20;

private:
    std::string_view m_text;
    std::uint64_t m_magnitude = 0;
    bool m_negative = false;
    bool m_isInteger = false;
};

/// The seed of the tuple (kSeedTupleTag, parts...): SHA-256 (rv/core/sha256.hpp, ADR-0008 §1: no other hash library)
/// over the tuple's elements separated by NUL characters (none after the last), the digest's first 8 bytes read
/// big-endian. With no part the tuple is the tag alone; an empty part is an element of its own ("rv-seed/1\0").
[[nodiscard]] std::uint64_t deriveSeed(std::span<const SeedPart> parts) noexcept;

/// deriveSeed() of the parts as given: `deriveSeed(worldSeed, "room", roomId)`.
template <class... Parts>
    requires(std::constructible_from<SeedPart, const Parts&> && ...)
[[nodiscard]] std::uint64_t deriveSeed(const Parts&... parts) noexcept {
    if constexpr (sizeof...(Parts) == 0) {
        return deriveSeed(std::span<const SeedPart>());
    } else {
        const SeedPart list[] = {SeedPart(parts)...};
        return deriveSeed(std::span<const SeedPart>(list));
    }
}

} // namespace rv::core
