#include <rv/core/sha256.hpp>

#include <algorithm>
#include <bit>

namespace rv::core {
namespace {

// FIPS 180-4 §4.2.2: the first 32 bits of the fractional parts of the cube roots of the first 64 primes.
constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// FIPS 180-4 §5.3.3: the first 32 bits of the fractional parts of the square roots of the first 8 primes.
constexpr std::array<std::uint32_t, 8> kInitialState{
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

constexpr std::uint32_t loadBigEndian(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

} // namespace

void Sha256::reset() noexcept {
    m_state = kInitialState;
    m_block.fill(0);
    m_blockFill = 0;
    m_messageBytes = 0;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t t = 0; t < 16; ++t) {
        schedule[t] = loadBigEndian(block + t * 4);
    }
    for (std::size_t t = 16; t < 64; ++t) {
        const std::uint32_t w15 = schedule[t - 15];
        const std::uint32_t w2 = schedule[t - 2];
        const std::uint32_t sigma0 = std::rotr(w15, 7) ^ std::rotr(w15, 18) ^ (w15 >> 3);
        const std::uint32_t sigma1 = std::rotr(w2, 17) ^ std::rotr(w2, 19) ^ (w2 >> 10);
        schedule[t] = sigma1 + schedule[t - 7] + sigma0 + schedule[t - 16];
    }

    std::uint32_t a = m_state[0];
    std::uint32_t b = m_state[1];
    std::uint32_t c = m_state[2];
    std::uint32_t d = m_state[3];
    std::uint32_t e = m_state[4];
    std::uint32_t f = m_state[5];
    std::uint32_t g = m_state[6];
    std::uint32_t h = m_state[7];
    for (std::size_t t = 0; t < 64; ++t) {
        const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + sum1 + choose + kRoundConstants[t] + schedule[t];
        const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::absorb(const std::uint8_t* data, std::size_t size) noexcept {
    m_messageBytes += size;
    if (m_blockFill > 0) {
        const std::size_t taken = std::min(size, m_block.size() - m_blockFill);
        std::copy_n(data, taken, m_block.data() + m_blockFill);
        m_blockFill += taken;
        data += taken;
        size -= taken;
        if (m_blockFill < m_block.size()) {
            return;
        }
        compress(m_block.data());
        m_blockFill = 0;
    }
    while (size >= m_block.size()) {
        compress(data);
        data += m_block.size();
        size -= m_block.size();
    }
    std::copy_n(data, size, m_block.data());
    m_blockFill = size;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
    absorb(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

void Sha256::update(std::string_view text) noexcept {
    absorb(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

Sha256Digest Sha256::finish() noexcept {
    // FIPS 180-4 §5.1.1: a 1 bit, zeros up to 56 bytes modulo 64, the message length in bits as 64-bit big endian.
    const std::uint64_t messageBits = m_messageBytes * 8u;
    m_block[m_blockFill++] = 0x80;
    if (m_blockFill > 56) {
        std::fill(m_block.begin() + static_cast<std::ptrdiff_t>(m_blockFill), m_block.end(), std::uint8_t{0});
        compress(m_block.data());
        m_blockFill = 0;
    }
    std::fill(m_block.begin() + static_cast<std::ptrdiff_t>(m_blockFill), m_block.begin() + 56, std::uint8_t{0});
    for (std::size_t i = 0; i < 8; ++i) {
        m_block[56 + i] = static_cast<std::uint8_t>(messageBits >> (56 - 8 * i));
    }
    compress(m_block.data());

    Sha256Digest digest{};
    for (std::size_t i = 0; i < m_state.size(); ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(m_state[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(m_state[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(m_state[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(m_state[i]);
    }
    reset();
    return digest;
}

Sha256Digest sha256(std::span<const std::byte> data) noexcept {
    Sha256 hash;
    hash.update(data);
    return hash.finish();
}

Sha256Digest sha256(std::string_view text) noexcept {
    Sha256 hash;
    hash.update(text);
    return hash.finish();
}

std::string toHex(std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text.push_back(kDigits[byte >> 4]);
        text.push_back(kDigits[byte & 0x0F]);
    }
    return text;
}

std::string sha256Hex(std::span<const std::byte> data) {
    return toHex(sha256(data));
}

std::string sha256Hex(std::string_view text) {
    return toHex(sha256(text));
}

} // namespace rv::core
