// Canonical JSON (rv/core/canonical_json.hpp, ADR-0010 §8) against golden texts and digests from an independent oracle:
// the table below is the output of `python -I scripts/world_seed_oracle.py` (Python's json and hashlib), pasted as
// printed; `python -I scripts/world_seed_oracle.py --check tests/unit/core/test_seed_random.cpp
// tests/unit/core/test_canonical_json.cpp` finds every one of its lines in the two files.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <rv/core/canonical_json.hpp>
#include <rv/core/json.hpp>
#include <rv/core/sha256.hpp>

using rv::core::canonicalJsonDigest;
using rv::core::parseCanonicalJson;
using rv::core::writeCanonicalJson;
using Json = nlohmann::json;

namespace {

struct CanonicalGolden {
    std::string_view input;     ///< any JSON text of the value
    std::string_view canonical; ///< its canonical text
    std::string_view digest;    ///< SHA-256 of the canonical text
};

// clang-format off
const CanonicalGolden kCanonical[] = {
{"0", "0\x0A", "9a271f2a916b0b6ee6cecb2426f0b3206ef074578be55d9bc94f6f3fe3ab86aa"},
{"-7", "-7\x0A", "71f24bf62be4ee171b9cf3b871966bb02bab0739dc01597afab02242a8378c03"},
{"18446744073709551615", "18446744073709551615\x0A", "f01cedc887ef3f800ffcf67e544b5b16cca680844255ccd759d5af6cf0ecd2e5"},
{"-9223372036854775808", "-9223372036854775808\x0A", "6597fb5664deb37b5413ea87a2566ffb9da2fb1376d820e4a40dd30c05aa8b04"},
{"true", "true\x0A", "a17fcf0a2f50e2d495e4f90ce263410edc183add6c62699a2facbccf60410f74"},
{"null", "null\x0A", "38e0b9de817f645c4bec37c0d4a3e58baecccb040f5718dc069a72c7385a0bed"},
{"\x22\x22", "\x22\x22\x0A", "bd85bcdb8d4e613a79cb62d0903946ad10c83e63dc75f67614c159c0dbf4d184"},
{"[]", "[]\x0A", "37517e5f3dc66819f61f5a7bb8ace1921282415f10551d2defa5c3eb0985b570"},
{"{}", "{}\x0A", "ca3d163bab055381827226140568f3bef7eaac187cebd76878e0b63e9e442356"},
{"[1, 2, 3]", "[\x0A" "1,\x0A" "2,\x0A" "3\x0A]\x0A", "7c0e5370b3aae846e796b95af384de15e99a933538dbdbe015af58d5e686ef41"},
{"[[1, 2], [], {\x22" "b\x22: 1, \x22" "a\x22: [3, 4]}]", "[\x0A[1,2],\x0A[],\x0A{\x22" "a\x22:[3,4],\x22" "b\x22:1}\x0A]\x0A", "c3a87cf30f77d0aeaa4ffe8dda11bbedebe16b2c73994c025024a84398a9500a"},
{"{\x22" "b\x22: 1, \x22" "a\x22: 2, \x22" "c\x22: {\x22z\x22: [], \x22y\x22: {}, \x22x\x22: [1, [2, 3]]}}", "{\x22" "a\x22:2,\x22" "b\x22:1,\x22" "c\x22:{\x22x\x22:[1,[2,3]],\x22y\x22:{},\x22z\x22:[]}}\x0A", "3c625fd318f457f58a8afd998e681fcc1fe375c30012f9d43018f08b3839e886"},
{"{\x22placements\x22: [{\x22slot\x22: \x22s:1.2\x22, \x22game\x22: \x22g0000000000000001\x22}, {\x22slot\x22: \x22s:0.0\x22, \x22game\x22: \x22g2\x22}], \x22schemaVersion\x22: 1, \x22" "empty\x22: [], \x22" "flags\x22: [true, false, null]}", "{\x22" "empty\x22:[],\x22" "flags\x22:[\x0Atrue,\x0A" "false,\x0Anull\x0A],\x22placements\x22:[\x0A{\x22game\x22:\x22g0000000000000001\x22,\x22slot\x22:\x22s:1.2\x22},\x0A{\x22game\x22:\x22g2\x22,\x22slot\x22:\x22s:0.0\x22}\x0A],\x22schemaVersion\x22:1}\x0A", "244c2b1f2128c5869b504ac325578e23e72c700af72e292f779a7dc8a4d6f6aa"},
{"{\x22" "a\x22: 1, \x22" "B\x22: 2, \x22_\x22: 3, \x22~\x22: 4, \x22\xC3\xA9\x22: 5, \x22" "e\xCC\x81\x22: 6, \x22\xF0\x9F\x8E\xAE\x22: 7, \x22\xEF\xBF\xBF\x22: 8, \x22\x22: 9}", "{\x22\x22:9,\x22" "B\x22:2,\x22_\x22:3,\x22" "a\x22:1,\x22" "e\xCC\x81\x22:6,\x22~\x22:4,\x22\xC3\xA9\x22:5,\x22\xEF\xBF\xBF\x22:8,\x22\xF0\x9F\x8E\xAE\x22:7}\x0A", "09f08c2228d936c27e0bc9eced8ffeb7b6df021995d0a7a7758f4e1b626bdf28"},
{"{\x22text\x22: \x22quote \x5C\x22 backslash \x5C\x5C slash / tab \x5Ct line \x5Cn return \x5Cr back \x5C" "b feed \x5C" "f\x22}", "{\x22text\x22:\x22quote \x5C\x22 backslash \x5C\x5C slash / tab \x5Ct line \x5Cn return \x5Cr back \x5C" "b feed \x5C" "f\x22}\x0A", "37a9d371f81f1cec3cdf3982208220c2f6bbb85544f4c12a158a8df91678f004"},
{"[\x22\x5Cu0000\x5Cu0001\x5Cu001f\x5Cu007f\x22, \x22\xC3\xA9\xE2\x80\xA8\xE2\x80\xA9\x22, \x22\x5Cud83c\x5Cudfae\x22]", "[\x0A\x22\x5Cu0000\x5Cu0001\x5Cu001f\x7F\x22,\x0A\x22\xC3\xA9\xE2\x80\xA8\xE2\x80\xA9\x22,\x0A\x22\xF0\x9F\x8E\xAE\x22\x0A]\x0A", "82365131a6cd199217478808f477a979b6e848607d2c16f9cced99d0a399710c"},
{"{\x22nested\x22: {\x22" "array\x22: [{\x22" "deep\x22: [1, 2]}]}, \x22top\x22: [{\x22" "b\x22: 2, \x22" "a\x22: 1}]}", "{\x22nested\x22:{\x22" "array\x22:[{\x22" "deep\x22:[1,2]}]},\x22top\x22:[\x0A{\x22" "a\x22:1,\x22" "b\x22:2}\x0A]}\x0A", "ef2a8d5ee645251b8bc559bd942d76014700dc4949593f9dd6995a28631e1fd1"},
};
// clang-format on

Json parsed(std::string_view text) {
    auto value = rv::core::parseJson(text, "test");
    REQUIRE(value.has_value());
    return *value;
}

std::string canonicalOf(const Json& value) {
    auto text = writeCanonicalJson(value);
    REQUIRE(text.has_value());
    return *text;
}

} // namespace

TEST_CASE("core/canonical_json: the oracle's golden texts and digests", "[core][canonical_json]") {
    for (const CanonicalGolden& row : kCanonical) {
        INFO(row.input);
        const Json value = parsed(row.input);
        CHECK(canonicalOf(value) == row.canonical);
        const auto digest = canonicalJsonDigest(value);
        REQUIRE(digest.has_value());
        CHECK(*digest == row.digest);
        CHECK(rv::core::sha256Hex(row.canonical) == row.digest);
    }
}

TEST_CASE("core/canonical_json: a read and write round trip is byte-identical", "[core][canonical_json]") {
    for (const CanonicalGolden& row : kCanonical) {
        INFO(row.canonical);
        const auto read = parseCanonicalJson(row.canonical, "golden");
        REQUIRE(read.has_value());
        CHECK(*read == parsed(row.input));
        CHECK(canonicalOf(*read) == row.canonical);
    }
    // A document built in code: written, read back, written again.
    Json plan = Json::object();
    plan["schemaVersion"] = 1;
    plan["placements"] = Json::array();
    for (int i = 3; i > 0; --i) {
        plan["placements"].push_back({{"slot", "room:snes/fx:w03/s:0." + std::to_string(i)}, {"lengthMm", -i * 125}});
    }
    plan["digests"] = {{"input", std::string(64, 'a')}};
    const std::string first = canonicalOf(plan);
    const auto read = parseCanonicalJson(first, "plan");
    REQUIRE(read.has_value());
    CHECK(canonicalOf(*read) == first);
    CHECK(first == "{\"digests\":{\"input\":\"" + std::string(64, 'a') +
                       "\"},\"placements\":[\n"
                       "{\"lengthMm\":-375,\"slot\":\"room:snes/fx:w03/s:0.3\"},\n"
                       "{\"lengthMm\":-250,\"slot\":\"room:snes/fx:w03/s:0.2\"},\n"
                       "{\"lengthMm\":-125,\"slot\":\"room:snes/fx:w03/s:0.1\"}\n"
                       "],\"schemaVersion\":1}\n");
}

TEST_CASE("core/canonical_json: integers only", "[core][canonical_json]") {
    CHECK(canonicalOf(Json(std::numeric_limits<std::int64_t>::min())) == "-9223372036854775808\n");
    CHECK(canonicalOf(Json(std::numeric_limits<std::uint64_t>::max())) == "18446744073709551615\n");

    const auto refusedAt = [](const Json& value, std::string_view pointer) {
        const auto text = writeCanonicalJson(value);
        REQUIRE_FALSE(text.has_value());
        CHECK(text.error().code == rv::ErrorCode::InvalidArgument);
        CHECK(text.error().context == pointer);
        CHECK(canonicalJsonDigest(value).error().code == rv::ErrorCode::InvalidArgument);
        return text.error().message;
    };
    CHECK(refusedAt(Json(0.5), "").find("floating-point") != std::string::npos);
    CHECK(refusedAt(Json(1.0), "").find("floating-point") != std::string::npos);
    refusedAt(Json{{"fixtures", {{{"id", "w01"}, {"x", 1.5}}}}}, "/fixtures/0/x");
    // Keys with '/' and '~' are escaped in the pointer (RFC 6901).
    refusedAt(Json{{"a/b", {{"c~d", 2.0}}}}, "/a~1b/c~0d");
    refusedAt(Json::binary({1, 2, 3}), "");
}

TEST_CASE("core/canonical_json: nesting is bounded", "[core][canonical_json]") {
    // 64 levels are written; a 65th is refused, and a text nested a hundred thousand levels deep is read without
    // overflowing the stack.
    const auto nested = [](int levels) {
        Json value = 0;
        for (int i = 0; i < levels; ++i) {
            value = Json::array({value});
        }
        return value;
    };
    CHECK(writeCanonicalJson(nested(64)).has_value());
    const auto deep = writeCanonicalJson(nested(65));
    REQUIRE_FALSE(deep.has_value());
    CHECK(deep.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(deep.error().message.find("nested deeper") != std::string::npos);
    const std::string text = std::string(100'000, '[') + std::string(100'000, ']') + "\n";
    const auto read = parseCanonicalJson(text, "deep.json");
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().code == rv::ErrorCode::ValidationFailed);
}

TEST_CASE("core/canonical_json: strings must be well-formed UTF-8", "[core][canonical_json]") {
    const auto invalid = writeCanonicalJson(Json{{"title", std::string("caf\xE9")}});
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(invalid.error().context == "/title");
    // A key: the pointer is not repeated (it holds the malformed text).
    Json badKey = Json::object();
    badKey[std::string("\xC3")] = 1;
    const auto key = writeCanonicalJson(badKey);
    REQUIRE_FALSE(key.has_value());
    CHECK(key.error().message.find("key") != std::string::npos);
    CHECK(rv::core::writeCanonicalJson(Json(std::string("\xF0\x9F\x8E\xAE"))).value() == "\"\xF0\x9F\x8E\xAE\"\n");
}

TEST_CASE("core/canonical_json: escapes", "[core][canonical_json]") {
    // Every control character: the five short forms, the others \u00XX in lowercase; DEL and '/' as they are.
    std::string controls;
    for (int c = 0; c < 0x20; ++c) {
        controls += static_cast<char>(c);
    }
    controls += "\x7F/\"\\";
    CHECK(canonicalOf(Json(controls)) ==
          "\"\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f"
          "\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e"
          "\\u001f\x7F/\\\"\\\\\"\n");
}

TEST_CASE("core/canonical_json: key order is code point order", "[core][canonical_json]") {
    // U+FFFF before U+10000: code points (UTF-8 bytes), where UTF-16 code units (RFC 8785) would put the surrogates of
    // U+10000 (D800) first.
    Json value = Json::object();
    value["\xF0\x90\x80\x80"] = 1; // U+10000
    value["\xEF\xBF\xBF"] = 2;     // U+FFFF
    value["Z"] = 3;
    value["a"] = 4;
    CHECK(canonicalOf(value) == "{\"Z\":3,\"a\":4,\"\xEF\xBF\xBF\":2,\"\xF0\x90\x80\x80\":1}\n");
}

TEST_CASE("core/canonical_json: only canonical texts are read", "[core][canonical_json]") {
    const auto refused = [](std::string_view text) {
        INFO(text);
        const auto value = parseCanonicalJson(text, "plan.json");
        REQUIRE_FALSE(value.has_value());
        CHECK(value.error().context == "plan.json");
        return value.error().code;
    };
    using rv::ErrorCode;
    CHECK(refused("{\"a\":1}") == ErrorCode::ValidationFailed);                 // no final line feed
    CHECK(refused("{\"a\": 1}\n") == ErrorCode::ValidationFailed);              // white space
    CHECK(refused("{\"b\":1,\"a\":2}\n") == ErrorCode::ValidationFailed);       // keys out of order
    CHECK(refused("{\"a\":1,\"a\":2}\n") == ErrorCode::ValidationFailed);       // a key twice
    CHECK(refused("{\"a\":[1,2]}\n") == ErrorCode::ValidationFailed);           // a top-level array on one line
    CHECK(refused("{\"a\":{\"b\":[\n1\n]}}\n") == ErrorCode::ValidationFailed); // a nested one on lines
    CHECK(refused("{\"a\":1.0}\n") == ErrorCode::ValidationFailed);             // a floating-point number
    CHECK(refused("{\"a\":-0}\n") == ErrorCode::ValidationFailed);              // another spelling of 0
    CHECK(refused("{\"a\":\"\\u0041\"}\n") == ErrorCode::ValidationFailed);     // an escape the form never writes
    CHECK(refused("{\"a\":\"\\/\"}\n") == ErrorCode::ValidationFailed);
    CHECK(refused("\xEF\xBB\xBF{}\n") == ErrorCode::ValidationFailed); // a byte order mark
    CHECK(refused("{\"a\":1}\n\n") == ErrorCode::ValidationFailed);    // a second line feed
    CHECK(refused("{\"a\":1} // comment\n") == ErrorCode::ParseError); // comments are not JSON
    CHECK(refused("{\"a\":") == ErrorCode::ParseError);
    CHECK(refused("") == ErrorCode::ParseError);
    // The byte where the text departs is named.
    const auto departs = parseCanonicalJson("{\"b\":1,\"a\":2}\n", "plan.json");
    CHECK(departs.error().message.find("at byte 2") != std::string::npos);
}
