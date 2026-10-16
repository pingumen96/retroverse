// SHA-256 against the FIPS 180-4 examples (NIST's "SHA-256 example" vectors) and digests computed by Python's hashlib
// at the padding boundaries; streaming in pieces of any size gives the one-shot digest.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <rv/core/sha256.hpp>

using rv::core::Sha256;
using rv::core::sha256Hex;
using rv::core::toHex;

TEST_CASE("core/sha256: the FIPS 180-4 vectors", "[core][sha256]") {
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 448 bits.
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // 896 bits.
    CHECK(sha256Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmn"
                    "opqrstnopqrstu") == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    // One million 'a'.
    CHECK(sha256Hex(std::string(1'000'000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("core/sha256: messages around the padding boundaries", "[core][sha256]") {
    // hashlib.sha256(b'a' * n): the length field fits in the last block (55), just does not (56), blocks end exactly.
    CHECK(sha256Hex(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(sha256Hex(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(sha256Hex(std::string(63, 'a')) == "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    CHECK(sha256Hex(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(sha256Hex(std::string(65, 'a')) == "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
    CHECK(sha256Hex(std::string(119, 'a')) == "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
    CHECK(sha256Hex(std::string(120, 'a')) == "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
    CHECK(sha256Hex(std::string(128, 'a')) == "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e");
}

TEST_CASE("core/sha256: streaming in pieces of any size gives the one-shot digest", "[core][sha256]") {
    // bytes((i * 31 + 7) & 0xFF for i in range(1000)), hashed by hashlib.
    std::vector<std::byte> message(1000);
    for (std::size_t i = 0; i < message.size(); ++i) {
        message[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
    }
    const std::string expected = "5097e7d587352f5097062ae679f37bda5802d9f875aba14c8cb4d1a188ada179";
    CHECK(sha256Hex(message) == expected);
    for (const std::size_t piece : {1u, 3u, 55u, 56u, 63u, 64u, 65u, 127u, 128u, 999u, 1000u}) {
        INFO("pieces of " << piece << " bytes");
        Sha256 hash;
        for (std::size_t offset = 0; offset < message.size(); offset += piece) {
            hash.update(std::span<const std::byte>(message).subspan(offset, std::min(piece, message.size() - offset)));
        }
        CHECK(toHex(hash.finish()) == expected);
    }

    // finish() starts a new message, reset() forgets one.
    Sha256 hash;
    hash.update("ab");
    hash.update("c");
    CHECK(toHex(hash.finish()) == sha256Hex("abc"));
    hash.update("abc");
    CHECK(toHex(hash.finish()) == sha256Hex("abc"));
    hash.update("something else");
    hash.reset();
    CHECK(toHex(hash.finish()) == sha256Hex(""));
}

TEST_CASE("core/sha256: digests and their text form", "[core][sha256]") {
    const rv::core::Sha256Digest digest = rv::core::sha256("abc");
    CHECK(digest.size() == 32);
    CHECK(digest[0] == 0xba);
    CHECK(digest[31] == 0xad);
    CHECK(toHex(digest) == sha256Hex("abc"));
    const std::vector<std::uint8_t> bytes{0x00, 0x0f, 0xa0, 0xff};
    CHECK(toHex(bytes) == "000fa0ff");
    CHECK(toHex(std::vector<std::uint8_t>{}).empty());
}
