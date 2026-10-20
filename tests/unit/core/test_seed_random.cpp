// Seeds (rv/core/seed.hpp) and the random generator (rv/core/random.hpp), ADR-0010 §7, against golden values from an
// independent oracle: the tables below are the output of `python -I scripts/world_seed_oracle.py` (Python's hashlib
// and its own SplitMix64, nothing shared with rv_core), pasted as printed; `python -I scripts/world_seed_oracle.py
// --check tests/unit/core/test_seed_random.cpp tests/unit/core/test_canonical_json.cpp` finds every one of its lines
// in the two files. A change of a definition changes both, and kRandomAlgorithm with it.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/random.hpp>
#include <rv/core/seed.hpp>
#include <rv/core/sha256.hpp>

using rv::core::deriveSeed;
using rv::core::Random;
using rv::core::SeedPart;

namespace {

constexpr std::size_t kDraws = 6;

struct SeedGolden {
    std::vector<std::string_view> parts;
    std::uint64_t seed;
};

struct ChainGolden {
    std::string_view name;
    std::string_view parent; ///< the line whose seed comes first in the tuple; empty: none
    std::vector<std::string_view> parts;
    std::uint64_t seed;
};

struct NextGolden {
    std::uint64_t seed;
    std::array<std::uint64_t, kDraws> values;
};

struct BelowGolden {
    std::uint64_t seed;
    std::uint64_t bound;
    std::array<std::uint64_t, kDraws> values;
    std::uint64_t state; ///< after the draws
};

struct BetweenGolden {
    std::uint64_t seed;
    std::int64_t low;
    std::int64_t high;
    std::array<std::int64_t, kDraws> values;
    std::uint64_t state;
};

struct UnitGolden {
    std::uint64_t seed;
    std::array<double, kDraws> values;
};

struct ShuffleGolden {
    std::uint64_t seed;
    std::size_t size;
    std::vector<int> order; ///< 0, 1, ... size - 1 shuffled
    std::uint64_t state;
};

// clang-format off
const SeedGolden kSeeds[] = {
{{}, 0xeda6b8b8d17e4992ull},
{{""}, 0xd40832d5bd06bd17ull},
{{"", ""}, 0xed80e75433a6624full},
{{"ab", "c"}, 0xf1f0d3fb66dd1dd5ull},
{{"a", "bc"}, 0x4841210b0b925202ull},
{{"abc"}, 0x712bdbb673bd9d01ull},
{{"world", "esde"}, 0xa1fac9cfdf9c5474ull},
{{"world", "main"}, 0x463f58b47dee6a1aull},
{{"room", "room:snes"}, 0xbde0b0f8f773d6b1ull},
{{"fixture", "room:snes/fx:w03"}, 0x8e0a67b7f08dcdc5ull},
{{"prop", "fx:w03/l:2"}, 0xffaa35c47c3ff9e5ull},
{{"place", "g3c374d5e06145c9b", "cartridge_home_16bit"}, 0xc2c5daee4b7d10b2ull},
{{"room", "room:snes:2"}, 0x2ef77ad85978abe5ull},
{{"room", "area:shared:gen4"}, 0x1b0345ee11247d80ull},
{{"room", "area:shared:gen4:2"}, 0xaae31405731e97b9ull},
{{"room", "area:arcade"}, 0x5177b64935ac618cull},
{{"room", "area:arcade:3"}, 0x1a8b57da8c9f187eull},
{{"fixture", "area:shared:gen4/fx:w03"}, 0xd8cf405932f1770aull},
{{"0"}, 0x64c2d13eae01526eull},
{{"-42"}, 0xedc605eddf5a14fdull},
{{"-9223372036854775808", "9223372036854775807", "18446744073709551615"}, 0xbbb932193862b033ull},
{{"caf\xC3\xA9"}, 0x7c67378015f03ce5ull},
{{"cafe\xCC\x81"}, 0x19300ae3069a61c3ull},
{{"\xF0\x9F\x8E\xAE arcade"}, 0x8074446854b26c54ull},
};

const ChainGolden kChain[] = {
{"world", "", {"world", "esde"}, 0xa1fac9cfdf9c5474ull},
{"room", "world", {"room", "room:snes"}, 0x1ab6a19d258c94f6ull},
{"fixture", "room", {"fixture", "room:snes/fx:w03"}, 0x9ad2fe9e05606ab3ull},
{"prop", "room", {"prop", "fx:w03/l:2"}, 0xa7191a90ef280353ull},
{"place", "room", {"place", "g3c374d5e06145c9b", "cartridge_home_16bit"}, 0xe33bedc98f6b310cull},
};

const NextGolden kNext[] = {
{0x0000000000000000ull, {0xe220a8397b1dcdafull, 0x6e789e6aa1b965f4ull, 0x06c45d188009454full, 0xf88bb8a8724c81ecull, 0x1b39896a51a8749bull, 0x53cb9f0c747ea2eaull}},
{0x0000000000000001ull, {0x910a2dec89025cc1ull, 0xbeeb8da1658eec67ull, 0xf893a2eefb32555eull, 0x71c18690ee42c90bull, 0x71bb54d8d101b5b9ull, 0xc34d0bff90150280ull}},
{0xffffffffffffffffull, {0xe4d971771b652c20ull, 0xe99ff867dbf682c9ull, 0x382ff84cb27281e9ull, 0x6d1db36ccba982d2ull, 0xb4a0472e578069aeull, 0xd31dadbda438bb33ull}},
{0x9e3779b97f4a7c15ull, {0x6e789e6aa1b965f4ull, 0x06c45d188009454full, 0xf88bb8a8724c81ecull, 0x1b39896a51a8749bull, 0x53cb9f0c747ea2eaull, 0x2c829abe1f4532e1ull}},
{0xa1fac9cfdf9c5474ull, {0x75082148182a4c15ull, 0x6c6ca9432864f4c7ull, 0xed369d11ebe7d3b7ull, 0x9cc09d42ccb12c77ull, 0x46800525e85e0aefull, 0x9783d46a93202943ull}},
};

const BelowGolden kBelow[] = {
{0xa1fac9cfdf9c5474ull, 0x0000000000000001ull, {0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, 0x0000000000000002ull, {0x0000000000000001ull, 0x0000000000000001ull, 0x0000000000000001ull, 0x0000000000000001ull, 0x0000000000000001ull, 0x0000000000000001ull}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, 0x0000000000000003ull, {0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000002ull, 0x0000000000000002ull, 0x0000000000000002ull, 0x0000000000000002ull}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, 0x0000000000000006ull, {0x0000000000000003ull, 0x0000000000000003ull, 0x0000000000000005ull, 0x0000000000000005ull, 0x0000000000000005ull, 0x0000000000000005ull}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, 0x000000000000000aull, {0x0000000000000003ull, 0x0000000000000005ull, 0x0000000000000005ull, 0x0000000000000005ull, 0x0000000000000005ull, 0x0000000000000007ull}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, 0x00000000000003e8ull, {0x0000000000000355ull, 0x000000000000032full, 0x000000000000037full, 0x000000000000028full, 0x0000000000000267ull, 0x0000000000000273ull}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, 0x0000000100000000ull, {0x00000000182a4c15ull, 0x000000002864f4c7ull, 0x00000000ebe7d3b7ull, 0x00000000ccb12c77ull, 0x00000000e85e0aefull, 0x0000000093202943ull}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, 0x8000000000000001ull, {0x6d369d11ebe7d3b6ull, 0x1cc09d42ccb12c76ull, 0x1783d46a93202942ull, 0x6932a4f5d6596e38ull, 0x1090419e68b6dab8ull, 0x6fe237e1c113e88eull}, 0x31ee1155593ab131ull},
{0xa1fac9cfdf9c5474ull, 0xffffffffffffffffull, {0x75082148182a4c15ull, 0x6c6ca9432864f4c7ull, 0xed369d11ebe7d3b7ull, 0x9cc09d42ccb12c77ull, 0x46800525e85e0aefull, 0x9783d46a93202943ull}, 0x5747a428db5b3cf2ull},
};

const BetweenGolden kBetween[] = {
{0xa1fac9cfdf9c5474ull, 0ll, 0ll, {0ll, 0ll, 0ll, 0ll, 0ll, 0ll}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, -5ll, 5ll, {-5ll, -3ll, -4ll, 2ll, 5ll, -1ll}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, 1ll, 6ll, {4ll, 4ll, 6ll, 6ll, 6ll, 6ll}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, 0ll, 4ll, {3ll, 0ll, 0ll, 0ll, 0ll, 2ll}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, -150ll, 150ll, {119ll, 3ll, 4ll, 123ll, 16ll, 41ll}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, (-9223372036854775807ll - 1), 9223372036854775807ll, {-790345141076734955ll, -1410566477368199993ll, 7869650099174364087ll, 2071828738817993847ll, -4143305996810450193ll, 1694431439021418819ll}, 0x5747a428db5b3cf2ull},
{0xa1fac9cfdf9c5474ull, (-9223372036854775807ll - 1), 0ll, {-1353721937680411722ll, -7151543298036781962ll, -7528940597833356990ll, -1643069538266681800ll, -8029845986984142152ll, -1161304311161689970ll}, 0x31ee1155593ab131ull},
{0xa1fac9cfdf9c5474ull, 10ll, 3ll, {10ll, 10ll, 10ll, 10ll, 10ll, 10ll}, 0xa1fac9cfdf9c5474ull},
};

const UnitGolden kUnit[] = {
{0x0000000000000000ull, {0x1.c4415072f63b9p-1, 0x1.b9e279aa86e58p-2, 0x1.b117462002500p-6, 0x1.f1177150e4990p-1, 0x1.b39896a51a870p-4, 0x1.4f2e7c31d1fa8p-2}},
{0x0000000000000001ull, {0x1.22145bd91204bp-1, 0x1.7dd71b42cb1ddp-1, 0x1.f12745ddf664ap-1, 0x1.c7061a43b90b2p-2, 0x1.c6ed53634406cp-2, 0x1.869a17ff202a0p-1}},
{0xffffffffffffffffull, {0x1.c9b2e2ee36ca5p-1, 0x1.d33ff0cfb7ed0p-1, 0x1.c17fc26593940p-3, 0x1.b476cdb32ea60p-2, 0x1.69408e5caf00dp-1, 0x1.a63b5b7b48717p-1}},
{0x9e3779b97f4a7c15ull, {0x1.b9e279aa86e58p-2, 0x1.b117462002500p-6, 0x1.f1177150e4990p-1, 0x1.b39896a51a870p-4, 0x1.4f2e7c31d1fa8p-2, 0x1.6414d5f0fa298p-3}},
{0xa1fac9cfdf9c5474ull, {0x1.d420852060a92p-2, 0x1.b1b2a50ca193cp-2, 0x1.da6d3a23d7cfap-1, 0x1.39813a8599625p-1, 0x1.1a001497a1782p-2, 0x1.2f07a8d526405p-1}},
};

const ShuffleGolden kShuffle[] = {
{0x0000000000000000ull, 0, {}, 0x0000000000000000ull},
{0x0000000000000000ull, 1, {0}, 0x0000000000000000ull},
{0x0000000000000000ull, 2, {0, 1}, 0x9e3779b97f4a7c15ull},
{0x0000000000000000ull, 10, {6, 3, 2, 9, 8, 1, 4, 7, 0, 5}, 0x8ff34785799e5cbdull},
{0x0000000000000001ull, 0, {}, 0x0000000000000001ull},
{0x0000000000000001ull, 1, {0}, 0x0000000000000001ull},
{0x0000000000000001ull, 2, {0, 1}, 0x9e3779b97f4a7c16ull},
{0x0000000000000001ull, 10, {4, 2, 8, 1, 9, 3, 0, 6, 7, 5}, 0x8ff34785799e5cbeull},
{0xffffffffffffffffull, 0, {}, 0xffffffffffffffffull},
{0xffffffffffffffffull, 1, {0}, 0xffffffffffffffffull},
{0xffffffffffffffffull, 2, {1, 0}, 0x9e3779b97f4a7c14ull},
{0xffffffffffffffffull, 10, {3, 4, 2, 7, 5, 0, 8, 1, 9, 6}, 0x8ff34785799e5cbcull},
{0x9e3779b97f4a7c15ull, 0, {}, 0x9e3779b97f4a7c15ull},
{0x9e3779b97f4a7c15ull, 1, {0}, 0x9e3779b97f4a7c15ull},
{0x9e3779b97f4a7c15ull, 2, {1, 0}, 0x3c6ef372fe94f82aull},
{0x9e3779b97f4a7c15ull, 10, {8, 7, 6, 5, 3, 9, 2, 4, 1, 0}, 0x2e2ac13ef8e8d8d2ull},
{0xa1fac9cfdf9c5474ull, 0, {}, 0xa1fac9cfdf9c5474ull},
{0xa1fac9cfdf9c5474ull, 1, {0}, 0xa1fac9cfdf9c5474ull},
{0xa1fac9cfdf9c5474ull, 2, {0, 1}, 0x403243895ee6d089ull},
{0xa1fac9cfdf9c5474ull, 10, {4, 9, 6, 1, 2, 5, 8, 7, 0, 3}, 0x31ee1155593ab131ull},
};
// clang-format on

std::uint64_t seedOf(const std::vector<std::string_view>& parts) {
    std::vector<SeedPart> list(parts.begin(), parts.end());
    return deriveSeed(std::span<const SeedPart>(list));
}

} // namespace

TEST_CASE("core/seed: the oracle's golden seeds", "[core][seed]") {
    for (const SeedGolden& row : kSeeds) {
        INFO("parts: " << row.parts.size() << (row.parts.empty() ? "" : ", first: " + std::string(row.parts[0])));
        CHECK(seedOf(row.parts) == row.seed);
    }
    // Two seeds of the table are never equal: the separators keep ("ab", "c") and ("a", "bc") apart, an empty part
    // counts, composed and decomposed text differ.
    std::map<std::uint64_t, std::size_t> distinct;
    for (const SeedGolden& row : kSeeds) {
        ++distinct[row.seed];
    }
    CHECK(distinct.size() == std::size(kSeeds));
}

TEST_CASE("core/seed: the tuple's bytes", "[core][seed]") {
    // "rv-seed/1", then each part after a NUL; the digest's first 8 bytes big-endian.
    const std::string tuple = std::string("rv-seed/1") + '\0' + "world" + '\0' + "esde";
    const rv::core::Sha256Digest digest = rv::core::sha256(tuple);
    std::uint64_t expected = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        expected = (expected << 8) | digest[i];
    }
    CHECK(deriveSeed("world", "esde") == expected);
    CHECK(deriveSeed() == seedOf({}));
    CHECK(rv::core::kSeedTupleTag == "rv-seed/1");
}

TEST_CASE("core/seed: integers are their decimal digits", "[core][seed]") {
    CHECK(deriveSeed(0) == deriveSeed("0"));
    CHECK(deriveSeed(-42) == deriveSeed("-42"));
    CHECK(deriveSeed(std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(),
                     std::numeric_limits<std::uint64_t>::max()) ==
          deriveSeed("-9223372036854775808", "9223372036854775807", "18446744073709551615"));
    CHECK(deriveSeed(std::uint8_t{200}, std::int8_t{-128}) == deriveSeed("200", "-128"));
    // Characters and booleans are no numbers.
    static_assert(!std::constructible_from<SeedPart, char>);
    static_assert(!std::constructible_from<SeedPart, bool>);
    static_assert(!std::constructible_from<SeedPart, char8_t>);
    static_assert(std::constructible_from<SeedPart, unsigned long long>);
    CHECK(deriveSeed(std::int16_t{-7}, std::uint32_t{7}) == deriveSeed("-7", "7"));
    // Every spelling of a text part is one part.
    const std::string owned = "room:snes";
    CHECK(deriveSeed(owned) == deriveSeed(std::string_view("room:snes")));
    CHECK(deriveSeed(owned) == deriveSeed("room:snes"));
    CHECK(deriveSeed(SeedPart(owned)) == deriveSeed(owned));
}

TEST_CASE("core/seed: the chain of the local seeds", "[core][seed]") {
    // ADR-0010 §7: world from the library, room from the world, fixtures, props and placements from the room, each
    // parent entering its children's tuples as a number.
    std::map<std::string_view, std::uint64_t> seeds;
    for (const ChainGolden& row : kChain) {
        INFO(row.name);
        std::uint64_t seed = 0;
        if (row.parent.empty()) {
            seed = seedOf(row.parts);
        } else {
            REQUIRE(seeds.contains(row.parent));
            std::vector<SeedPart> parts{SeedPart(seeds.at(row.parent))};
            parts.insert(parts.end(), row.parts.begin(), row.parts.end());
            seed = deriveSeed(std::span<const SeedPart>(parts));
        }
        CHECK(seed == row.seed);
        seeds[row.name] = seed;
    }
    // The variadic form as the generator writes it.
    const std::uint64_t world = deriveSeed("world", "esde");
    const std::uint64_t room = deriveSeed(world, "room", "room:snes");
    CHECK(room == seeds.at("room"));
    CHECK(deriveSeed(room, "place", "g3c374d5e06145c9b", "cartridge_home_16bit") == seeds.at("place"));
}

TEST_CASE("core/random: the oracle's golden sequences", "[core][random]") {
    CHECK(rv::core::kRandomAlgorithm == "splitmix64/1");
    for (const NextGolden& row : kNext) {
        INFO("seed " << row.seed);
        Random random(row.seed);
        for (const std::uint64_t value : row.values) {
            CHECK(random.next() == value);
        }
        CHECK(random.state() == row.seed + kDraws * 0x9E3779B97F4A7C15ull);
    }
    // Vigna's splitmix64.c from a state of 0 gives 0xE220A8397B1DCDAF first (the published reference).
    CHECK(Random(0).next() == 0xE220A8397B1DCDAFull);
    // Copies go on alike: the state is the whole generator.
    Random first(kNext[4].seed);
    static_cast<void>(first.next());
    Random copy = first;
    CHECK(copy.next() == first.next());
}

TEST_CASE("core/random: below() against the oracle", "[core][random]") {
    for (const BelowGolden& row : kBelow) {
        INFO("bound " << row.bound);
        Random random(row.seed);
        for (const std::uint64_t value : row.values) {
            const std::uint64_t drawn = random.below(row.bound);
            CHECK(drawn == value);
            CHECK(drawn < row.bound);
        }
        CHECK(random.state() == row.state);
    }
    // A bound of 0 gives 0 and draws nothing; a bound of 1 draws one output.
    Random random(1);
    CHECK(random.below(0) == 0);
    CHECK(random.state() == 1);
    CHECK(random.below(1) == 0);
    CHECK(random.state() == 1 + 0x9E3779B97F4A7C15ull);
}

TEST_CASE("core/random: between() against the oracle", "[core][random]") {
    for (const BetweenGolden& row : kBetween) {
        INFO("[" << row.low << ", " << row.high << "]");
        Random random(row.seed);
        for (const std::int64_t value : row.values) {
            const std::int64_t drawn = random.between(row.low, row.high);
            CHECK(drawn == value);
            if (row.low <= row.high) {
                CHECK(drawn >= row.low);
                CHECK(drawn <= row.high);
            }
        }
        CHECK(random.state() == row.state);
    }
}

TEST_CASE("core/random: unit() against the oracle", "[core][random]") {
    for (const UnitGolden& row : kUnit) {
        INFO("seed " << row.seed);
        Random random(row.seed);
        for (const double value : row.values) {
            const double drawn = random.unit();
            CHECK(drawn == value); // exact: the same bits
            CHECK(drawn >= 0.0);
            CHECK(drawn < 1.0);
        }
    }
}

TEST_CASE("core/random: shuffle() against the oracle", "[core][random]") {
    for (const ShuffleGolden& row : kShuffle) {
        INFO("seed " << row.seed << ", " << row.size << " items");
        std::vector<int> items(row.size);
        std::iota(items.begin(), items.end(), 0);
        Random random(row.seed);
        random.shuffle(std::span<int>(items));
        CHECK(items == row.order);
        CHECK(random.state() == row.state);
    }
    // Any item type: strings move as a whole.
    std::vector<std::string> names{"w01", "e01", "w02", "e02"};
    Random random(kShuffle[0].seed);
    random.shuffle(std::span<std::string>(names));
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == std::vector<std::string>{"e01", "e02", "w01", "w02"});
}

TEST_CASE("core/random: below() is uniform", "[core][random]") {
    // 60,000 draws of a die: each face within 3% of 10,000 (about 9 standard deviations of the binomial).
    Random random(deriveSeed("test", "uniform"));
    std::array<int, 6> faces{};
    for (int i = 0; i < 60'000; ++i) {
        ++faces[static_cast<std::size_t>(random.below(6))];
    }
    for (const int count : faces) {
        CHECK(count > 9'700);
        CHECK(count < 10'300);
    }
    // A bound just above 2^63 rejects about half the outputs and still lands on both halves alike.
    const std::uint64_t bound = (std::uint64_t{1} << 63) + 1;
    int low = 0;
    for (int i = 0; i < 10'000; ++i) {
        low += random.below(bound) < bound / 2 ? 1 : 0;
    }
    CHECK(low > 4'700);
    CHECK(low < 5'300);
}
