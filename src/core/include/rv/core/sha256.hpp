#pragma once

// SHA-256 (FIPS 180-4), streaming and one-shot: the identities of the catalog (GameId, ADR-0008 §4) and the quick hash
// of ROM files.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace rv::core {

/// A SHA-256 digest: 32 bytes, most significant byte of the first word first.
using Sha256Digest = std::array<std::uint8_t, 32>;

/// Streaming SHA-256: update() with the message in pieces of any size, then finish(), which returns the digest and
/// starts a new, empty message.
class Sha256 {
public:
    Sha256() noexcept { reset(); }

    void update(std::span<const std::byte> data) noexcept;
    void update(std::string_view text) noexcept;
    [[nodiscard]] Sha256Digest finish() noexcept;
    /// Forgets what was hashed so far.
    void reset() noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;
    void absorb(const std::uint8_t* data, std::size_t size) noexcept;

    std::array<std::uint32_t, 8> m_state{};
    std::array<std::uint8_t, 64> m_block{};
    std::size_t m_blockFill = 0;
    std::uint64_t m_messageBytes = 0;
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data) noexcept;
[[nodiscard]] Sha256Digest sha256(std::string_view text) noexcept;

/// Lowercase hexadecimal, two digits per byte.
[[nodiscard]] std::string toHex(std::span<const std::uint8_t> bytes);

/// toHex(sha256(...)).
[[nodiscard]] std::string sha256Hex(std::span<const std::byte> data);
[[nodiscard]] std::string sha256Hex(std::string_view text);

} // namespace rv::core
