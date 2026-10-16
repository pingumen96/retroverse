#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>

using rv::core::findInvalidUtf8;
using rv::core::isValidUtf8;
using rv::core::utf8ToWide;
using rv::core::wideToUtf8;

namespace {

constexpr wchar_t kHighSurrogate = static_cast<wchar_t>(0xD83C);
constexpr wchar_t kLowSurrogate = static_cast<wchar_t>(0xDFAE);

} // namespace

TEST_CASE("core/utf8: well-formed text", "[core][utf8]") {
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("ascii"));
    CHECK(isValidUtf8("\xC3\xA9\xE6\x97\xA5\xF0\x9F\x8E\xAE")); // é 日 🎮
    CHECK(isValidUtf8(std::string("nul\0inside", 10)));         // NUL is a character
    CHECK(isValidUtf8("\xF4\x8F\xBF\xBF"));                      // U+10FFFF, the last code point
    CHECK(isValidUtf8("\xED\x9F\xBF"));                          // U+D7FF, just below the surrogates
    CHECK_FALSE(findInvalidUtf8("Pok\xC3\xA9mon").has_value());
}

TEST_CASE("core/utf8: the offset of the first malformed sequence", "[core][utf8]") {
    CHECK(findInvalidUtf8("abc\x80") == 3u);              // a continuation byte without a lead byte
    CHECK(findInvalidUtf8("ab\xC3") == 2u);               // truncated at the end
    CHECK(findInvalidUtf8("\xC3\x28 rest") == 0u);        // a lead byte without its continuation
    CHECK(findInvalidUtf8("x\xC0\xAF") == 1u);            // overlong '/'
    CHECK(findInvalidUtf8("\xE0\x80\xAF") == 0u);         // overlong
    CHECK(findInvalidUtf8("\xF0\x80\x80\xAF") == 0u);     // overlong
    CHECK(findInvalidUtf8("ok \xED\xA0\x80") == 3u);      // a UTF-16 surrogate
    CHECK(findInvalidUtf8("\xF4\x90\x80\x80") == 0u);     // above U+10FFFF
    CHECK(findInvalidUtf8("\xF8\x88\x80\x80\x80") == 0u); // a 5-byte form
    CHECK(findInvalidUtf8("\xC3\xA9\xFF") == 2u);         // FF never appears
    CHECK(findInvalidUtf8("Caf\xE9 au lait") == 3u);      // Windows-1252 é
}

TEST_CASE("core/utf8: the launcher's check is the core one", "[core][utf8]") {
    bool (*launcherCheck)(std::string_view) noexcept = &rv::launcher::isValidUtf8;
    bool (*coreCheck)(std::string_view) noexcept = &rv::core::isValidUtf8;
    CHECK((launcherCheck == coreCheck));
}

TEST_CASE("core/utf8: conversions to and from UTF-16", "[core][utf8]") {
    const std::string text = "Pok\xC3\xA9mon \xE6\x97\xA5 \xF0\x9F\x8E\xAE";
    const auto wide = utf8ToWide(text);
    REQUIRE(wide.has_value());
    CHECK(*wide == std::wstring(L"Pokémon 日 ") + kHighSurrogate + kLowSurrogate);
    const auto back = wideToUtf8(*wide);
    REQUIRE(back.has_value());
    CHECK(*back == text);
    CHECK(utf8ToWide("").value().empty());
    CHECK(wideToUtf8(L"").value().empty());
    CHECK(utf8ToWide(std::string("a\0b", 3)).value() == std::wstring(L"a\0b", 3));

    const auto malformed = utf8ToWide("ab\xC3");
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(malformed.error().message.find("byte offset 2") != std::string::npos);
}

TEST_CASE("core/utf8: unpaired surrogates are errors, or U+FFFD where lossy", "[core][utf8]") {
    const std::wstring loneHigh = std::wstring(L"a") + kHighSurrogate + L"b";
    const std::wstring loneLow = std::wstring(L"a") + kLowSurrogate;
    const std::wstring highAtEnd = std::wstring(L"ab") + kHighSurrogate;
    const std::wstring reversed = std::wstring() + kLowSurrogate + kHighSurrogate;
    for (const std::wstring& text : {loneHigh, loneLow, highAtEnd, reversed}) {
        const auto converted = wideToUtf8(text);
        REQUIRE_FALSE(converted.has_value());
        CHECK(converted.error().code == rv::ErrorCode::InvalidArgument);
    }
    CHECK(wideToUtf8(loneLow).error().message.find("offset 1") != std::string::npos);
    CHECK(rv::core::wideToUtf8Lossy(loneHigh) == "a\xEF\xBF\xBD" "b");
    CHECK(rv::core::wideToUtf8Lossy(reversed) == "\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(rv::core::wideToUtf8Lossy(std::wstring() + kHighSurrogate + kLowSurrogate) == "\xF0\x9F\x8E\xAE");
}

TEST_CASE("core/utf8: path conversions without exceptions", "[core][utf8]") {
    const auto path = rv::core::tryPathFromUtf8("roms/Pok\xC3\xA9mon.min");
    REQUIRE(path.has_value());
    CHECK(path->filename().wstring() == L"Pokémon.min");
    CHECK(rv::core::tryPathToUtf8(*path).value() == "roms/Pok\xC3\xA9mon.min");
    CHECK(rv::core::tryPathFromUtf8("bad\xC3").error().code == rv::ErrorCode::InvalidArgument);

    // A name NTFS accepts but UTF-8 cannot hold: no exception, an error or U+FFFD.
    const std::filesystem::path unpaired(std::wstring(L"name") + kHighSurrogate + L".gb");
    const auto strict = rv::core::tryPathToUtf8(unpaired);
    REQUIRE_FALSE(strict.has_value());
    CHECK(strict.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(rv::core::pathToUtf8Lossy(unpaired) == "name\xEF\xBF\xBD.gb");
}

TEST_CASE("core/utf8: Windows-1252 text and the fallback for text that is not UTF-8", "[core][utf8]") {
    using rv::core::utf8WithWindows1252Fallback;
    using rv::core::windows1252ToUtf8;
    CHECK(windows1252ToUtf8("Caf\xE9 \x80 \x93quoted\x94 \x9F") ==
          "Caf\xC3\xA9 \xE2\x82\xAC \xE2\x80\x9Cquoted\xE2\x80\x9D \xC5\xB8"); // é € “ ” Ÿ
    CHECK(windows1252ToUtf8("\x81\x8D\x8F\x90\x9D") == "\xC2\x81\xC2\x8D\xC2\x8F\xC2\x90\xC2\x9D"); // undefined: C1
    CHECK(windows1252ToUtf8("\xC3\xA9") == "\xC3\x83\xC2\xA9");                                       // "Ã©"
    CHECK(windows1252ToUtf8(std::string_view("a\0b", 3)) == std::string("a\0b", 3));
    CHECK(windows1252ToUtf8("").empty());

    std::size_t fallback = 99;
    // Well-formed UTF-8 is kept as it is.
    CHECK(utf8WithWindows1252Fallback("Pok\xC3\xA9mon \xF0\x9F\x8E\xAE", &fallback) ==
          "Pok\xC3\xA9mon \xF0\x9F\x8E\xAE");
    CHECK(fallback == 0);
    // A file saved as ANSI reads as intended, and so do the bytes an ANSI editor added to a UTF-8 file.
    CHECK(utf8WithWindows1252Fallback("Caf\xE9 \xA9 1990", &fallback) == "Caf\xC3\xA9 \xC2\xA9 1990");
    CHECK(fallback == 2);
    CHECK(utf8WithWindows1252Fallback("Pok\xC3\xA9mon, Caf\xE9", &fallback) == "Pok\xC3\xA9mon, Caf\xC3\xA9");
    CHECK(fallback == 1);
    // Truncated, overlong and surrogate sequences: each of their bytes on its own.
    CHECK(utf8WithWindows1252Fallback("a\xC3", &fallback) == "a\xC3\x83");
    CHECK(fallback == 1);
    CHECK(utf8WithWindows1252Fallback("\xC0\xAF\xED\xA0\x80", &fallback) ==
          "\xC3\x80\xC2\xAF\xC3\xAD\xC2\xA0\xE2\x82\xAC");
    CHECK(fallback == 5);
    CHECK(isValidUtf8(utf8WithWindows1252Fallback(std::string_view("\xFF\xFE\x00\x80\xBF", 5))));
    CHECK(utf8WithWindows1252Fallback("").empty());
}
