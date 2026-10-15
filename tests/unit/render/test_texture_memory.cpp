#include <catch2/catch_test_macros.hpp>

#include <cstddef>

#include <rv/render/texture_memory.hpp>

TEST_CASE("render/texture_memory: a full chain halves each side down to 1 x 1", "[render][texture]") {
    CHECK(rv::gl::fullMipLevelCount(glm::ivec2(1, 1)) == 1);
    CHECK(rv::gl::fullMipLevelCount(glm::ivec2(2, 1)) == 2);
    CHECK(rv::gl::fullMipLevelCount(glm::ivec2(256, 256)) == 9);
    CHECK(rv::gl::fullMipLevelCount(glm::ivec2(300, 17)) == 9); // floor(log2(300)) + 1
    CHECK(rv::gl::fullMipLevelCount(glm::ivec2(0, 0)) == 1);
}

TEST_CASE("render/texture_memory: the chain of an RGBA8 texture takes about 4/3 of its base level",
          "[render][texture]") {
    CHECK(rv::gl::mipChainBytes(glm::ivec2(4, 4), 1) == 64);
    CHECK(rv::gl::mipChainBytes(glm::ivec2(4, 4), 3) == 64 + 16 + 4);
    // Non-square: the shorter side stops at 1 while the longer one keeps halving.
    CHECK(rv::gl::mipChainBytes(glm::ivec2(4, 1), 3) == 16 + 8 + 4);
    const glm::ivec2 size(1024, 1024);
    const std::size_t full = rv::gl::mipChainBytes(size, rv::gl::fullMipLevelCount(size));
    // 11 levels of 4^10, 4^9, ..., 1 texels: 4 bytes * (4^11 - 1) / 3.
    CHECK(full == 4u * ((std::size_t{1} << 22) - 1u) / 3u);
    CHECK(rv::gl::mipChainBytes(glm::ivec2(0, 8), 4) == 0);
    CHECK(rv::gl::mipChainBytes(size, 0) == 0);
}
