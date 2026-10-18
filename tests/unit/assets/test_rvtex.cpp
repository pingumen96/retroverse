// I3 step 5: the collection's layers and their .rvtex file (rv/assets/rvtex.hpp, ADR-0011 §4 and §7).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include <rv/assets/bc1.hpp>
#include <rv/assets/rvtex.hpp>

#include "cover_test_support.hpp"

using namespace rv::assets;
using rv::test::covers::makeCover;

namespace {

RvTexture frontLayer(TextureFormat format = TextureFormat::Bc1Srgb) {
    const LayerSize size = layerSize(TextureClass::Front, format);
    auto texture = makeLayerTexture(makeCover(size.width, size.height, 11, 4), TextureClass::Front, format,
                                    Rgb8{0x12, 0x34, 0x56});
    REQUIRE(texture.has_value());
    return std::move(*texture);
}

rv::core::Sha256Digest keyOf(std::string_view text) {
    return rv::core::sha256(text);
}

} // namespace

TEST_CASE("assets/rvtex: the classes' sizes and bytes match ADR-0011", "[assets][rvtex]") {
    CHECK(layerSize(TextureClass::Front, TextureFormat::Bc1Srgb) == LayerSize{256, 188});
    CHECK(layerSize(TextureClass::Spine, TextureFormat::Bc1Srgb) == LayerSize{40, 256});
    CHECK(layerSize(TextureClass::Front, TextureFormat::Rgba8Srgb) == LayerSize{128, 94});
    CHECK(layerSize(TextureClass::Spine, TextureFormat::Rgba8Srgb) == LayerSize{20, 128});
    // 32 KiB per front and 7 KiB per spine in BC1 with their mips: 39 KiB per item (ADR-0011 §9).
    CHECK(layerBytes(TextureClass::Front, TextureFormat::Bc1Srgb) == 32280);
    CHECK(layerBytes(TextureClass::Spine, TextureFormat::Bc1Srgb) == 6984);
    // The RGBA8 fallback at half the resolution takes about twice BC1's memory at full resolution.
    const std::size_t rgba = layerBytes(TextureClass::Front, TextureFormat::Rgba8Srgb) +
                             layerBytes(TextureClass::Spine, TextureFormat::Rgba8Srgb);
    const std::size_t bc1 = layerBytes(TextureClass::Front, TextureFormat::Bc1Srgb) +
                            layerBytes(TextureClass::Spine, TextureFormat::Bc1Srgb);
    CHECK(rgba > bc1 * 3 / 2);
    CHECK(rgba < bc1 * 5 / 2);
    CHECK(toString(TextureClass::Spine) == "spine");
    CHECK(toString(TextureFormat::Rgba8Srgb) == "rgba8");
}

TEST_CASE("assets/rvtex: a layer holds its full chain in its format", "[assets][rvtex]") {
    for (const TextureFormat format : {TextureFormat::Bc1Srgb, TextureFormat::Rgba8Srgb}) {
        const RvTexture texture = frontLayer(format);
        const LayerSize size = layerSize(TextureClass::Front, format);
        CHECK(texture.width == size.width);
        CHECK(texture.levels.size() == static_cast<std::size_t>(mipLevelCount(size.width, size.height)));
        CHECK(texture.payloadBytes() == layerBytes(TextureClass::Front, format));
        CHECK(decodeLevel(texture, 0).width == size.width);
        CHECK(decodeLevel(texture, texture.levels.size() - 1).width == 1);
        CHECK_FALSE(decodeLevel(texture, texture.levels.size()).isValid());
    }
    auto wrong = makeLayerTexture(makeCover(255, 188, 1, 4), TextureClass::Front, TextureFormat::Bc1Srgb, {});
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().code == rv::ErrorCode::InvalidArgument);
    CHECK_FALSE(makeLayerTexture(makeCover(256, 188, 1, 3), TextureClass::Front, TextureFormat::Bc1Srgb, {}));
}

TEST_CASE("assets/rvtex: a file round-trips byte for byte", "[assets][rvtex]") {
    RvTexture texture = frontLayer();
    texture.source = SourceStamp{530'123, 133'000'000'000'000'000LL};
    const auto key = keyOf("a key");
    auto bytes = serializeRvtex(texture, key);
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() == kRvtexHeaderBytes + 32280);
    auto parsed = parseRvtex(*bytes, &key, "entry");
    REQUIRE(parsed.has_value());
    CHECK(parsed->textureClass == texture.textureClass);
    CHECK(parsed->format == texture.format);
    CHECK(parsed->dominant == texture.dominant);
    CHECK(parsed->source == texture.source);
    REQUIRE(parsed->levels.size() == texture.levels.size());
    for (std::size_t level = 0; level < texture.levels.size(); ++level) {
        CHECK(parsed->levels[level].data == texture.levels[level].data);
    }
    auto again = serializeRvtex(*parsed, key);
    REQUIRE(again.has_value());
    CHECK(*again == *bytes);

    // A generated layer has no source stamp.
    RvTexture generated = frontLayer(TextureFormat::Rgba8Srgb);
    auto generatedBytes = serializeRvtex(generated, key);
    REQUIRE(generatedBytes.has_value());
    auto generatedBack = parseRvtex(*generatedBytes, nullptr, "entry");
    REQUIRE(generatedBack.has_value());
    CHECK_FALSE(generatedBack->source.has_value());
}

TEST_CASE("assets/rvtex: every damaged file is refused", "[assets][rvtex]") {
    const RvTexture texture = frontLayer();
    const auto key = keyOf("a key");
    const std::vector<std::byte> good = serializeRvtex(texture, key).value();
    const auto refused = [&](std::vector<std::byte> bytes, const rv::core::Sha256Digest* expected = nullptr) {
        auto parsed = parseRvtex(bytes, expected, "entry");
        return !parsed.has_value() && parsed.error().code == rv::ErrorCode::ValidationFailed;
    };
    CHECK(refused({}));
    CHECK(refused(std::vector<std::byte>(good.begin(), good.begin() + 95)));            // shorter than its header
    CHECK(refused(std::vector<std::byte>(good.begin(), good.end() - 1)));               // truncated payload
    auto longer = good;
    longer.push_back(std::byte{0});
    CHECK(refused(longer));                                                             // trailing bytes
    auto magic = good;
    magic[1] = std::byte{'X'};
    CHECK(refused(magic));
    auto payload = good;
    payload[kRvtexHeaderBytes + 1000] ^= std::byte{0x01};
    CHECK(refused(payload));                                                            // payload checksum
    auto header = good;
    header[20] ^= std::byte{0x01};
    CHECK(refused(header));                                                             // header checksum
    const auto otherKey = keyOf("another key");
    CHECK(refused(good, &otherKey));                                                    // another entry's file
    CHECK(parseRvtex(good, &key, "entry").has_value());

    // A texture whose chain does not match its class is never written.
    RvTexture broken = texture;
    broken.levels.pop_back();
    auto written = serializeRvtex(broken, key);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == rv::ErrorCode::InvalidArgument);
}
