// File-system case folding (ADR-0008 §4). The expectations record what LCMapStringEx (LOCALE_NAME_INVARIANT,
// LCMAP_UPPERCASE) does on Windows 11 and check it against CompareStringOrdinal, which compares names as the file
// systems do: they pin the behaviour the lookup keys rely on, so that a change of the system's casing table shows here
// (and casingTableVersion() changes with it).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <rv/core/case_fold.hpp>
#include <rv/core/utf8.hpp>

using rv::core::fileSystemUpperCase;

namespace {

std::string upper(std::string_view utf8) {
    const auto folded = fileSystemUpperCase(utf8);
    REQUIRE(folded.has_value());
    return *folded;
}

std::wstring upperWide(std::wstring_view utf16) {
    const auto folded = fileSystemUpperCase(utf16);
    REQUIRE(folded.has_value());
    return *folded;
}

/// CompareStringOrdinal ignoring case: how Windows compares file names, the oracle of the keys.
int compareIgnoringCase(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE);
}

} // namespace

TEST_CASE("core/case_fold: ASCII paths", "[core][case_fold]") {
    CHECK(upper("./sub/Game (USA).sfc") == "./SUB/GAME (USA).SFC");
    CHECK(upper("D:\\Roms\\Gb\\a.gb") == "D:\\ROMS\\GB\\A.GB");
    CHECK(upper("") == "");
    CHECK(upper("0123456789 !#$%&'()+,-.;=@[]^_`{}~") == "0123456789 !#$%&'()+,-.;=@[]^_`{}~");

    // The ASCII shortcut agrees with the system call: the same text in UTF-16 goes through LCMapStringEx.
    std::string ascii;
    std::wstring wide;
    for (int c = 1; c < 0x80; ++c) {
        ascii.push_back(static_cast<char>(c));
        wide.push_back(static_cast<wchar_t>(c));
    }
    const std::wstring system = upperWide(wide);
    const std::string shortcut = upper(ascii);
    REQUIRE(shortcut.size() == system.size());
    for (std::size_t i = 0; i < shortcut.size(); ++i) {
        INFO("character " << i + 1);
        CHECK(static_cast<wchar_t>(static_cast<unsigned char>(shortcut[i])) == system[i]);
    }
}

TEST_CASE("core/case_fold: Latin letters with diacritics", "[core][case_fold]") {
    CHECK(upper("\xC3\xA9") == "\xC3\x89");                                // é -> É
    CHECK(upper("Pok\xC3\xA9mon Party Mini (NA).min") == "POK\xC3\x89MON PARTY MINI (NA).MIN");
    CHECK(upper("Dynamite D\xC3\xBCx") == "DYNAMITE D\xC3\x9CX");          // ü -> Ü
    CHECK(upper("\xC3\xBF") == "\xC5\xB8");                                // ÿ -> Ÿ
    CHECK(upper("Espa\xC3\xB1" "a") == "ESPA\xC3\x91" "A");                // ñ -> Ñ
    CHECK(upper("1080\xC2\xB0 Avalanche \xC2\xBE") == "1080\xC2\xB0 AVALANCHE \xC2\xBE"); // ° and ¾ have no case
}

TEST_CASE("core/case_fold: no full case mapping, no linguistic casing", "[core][case_fold]") {
    // Sharp s has no single-character upper case: it stays (a full mapping would give "SS").
    CHECK(upper("Stra\xC3\x9F" "e") == "STRA\xC3\x9F" "E");
    // The file-system table leaves the letters whose upper case another letter shares: dotless i stays (Unicode's
    // simple mapping would give I) and a dotted i is never Turkish İ; final sigma stays while sigma becomes capital
    // sigma (observed).
    CHECK(upper("\xC4\xB1") == "\xC4\xB1");
    CHECK(upper("i") == "I");
    CHECK(upper("\xCF\x82") == "\xCF\x82");
    CHECK(upper("\xCF\x83") == "\xCE\xA3");
}

TEST_CASE("core/case_fold: NFC and NFD forms of a name stay different", "[core][case_fold]") {
    const std::string composed = "Caf\xC3\xA9";    // U+00E9
    const std::string decomposed = "Cafe\xCC\x81"; // e + U+0301 combining acute
    CHECK(upper(composed) == "CAF\xC3\x89");
    CHECK(upper(decomposed) == "CAFE\xCC\x81");
    CHECK(upper(composed) != upper(decomposed));
}

TEST_CASE("core/case_fold: characters outside the Basic Multilingual Plane", "[core][case_fold]") {
    // A game controller emoji has no case.
    CHECK(upper("\xF0\x9F\x8E\xAE.gb") == "\xF0\x9F\x8E\xAE.GB");
    // Deseret small letter long i (U+10428, a surrogate pair in UTF-16) keeps its case: the file systems map code
    // units, so a file named with it and one named with the capital (U+10400) are two files on NTFS, which
    // LCMapStringEx alone would fold to one key.
    CHECK(upper("\xF0\x90\x90\xA8") == "\xF0\x90\x90\xA8");
    CHECK(upper("x\xF0\x90\x90\xA8y\xC3\xA9") == "X\xF0\x90\x90\xA8Y\xC3\x89");
    CHECK(upper("\xF0\x90\x90\xA8") != upper("\xF0\x90\x90\x80"));
}

TEST_CASE("core/case_fold: equal keys exactly where Windows compares names as equal", "[core][case_fold]") {
    // Every code unit of the BMP but the surrogates: its key is one code unit that CompareStringOrdinal holds equal
    // to it...
    std::vector<wchar_t> keys;
    for (std::uint32_t unit = 0; unit <= 0xFFFF; ++unit) {
        if (unit >= 0xD800 && unit <= 0xDFFF) {
            continue;
        }
        const std::wstring text(1, static_cast<wchar_t>(unit));
        const std::wstring key = upperWide(text);
        REQUIRE(key.size() == 1);
        if (compareIgnoringCase(text, key) != CSTR_EQUAL) {
            FAIL_CHECK("U+" << std::hex << unit << " and its key U+" << static_cast<std::uint32_t>(key[0])
                            << " differ for CompareStringOrdinal");
        }
        keys.push_back(key[0]);
    }
    // ...and two different keys are never held equal (ordered as CompareStringOrdinal orders them, equal keys would be
    // neighbours): the keys split the BMP into the classes of the file systems' comparison.
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::stable_sort(keys.begin(), keys.end(), [](wchar_t a, wchar_t b) {
        return compareIgnoringCase(std::wstring_view(&a, 1), std::wstring_view(&b, 1)) == CSTR_LESS_THAN;
    });
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (compareIgnoringCase(std::wstring_view(&keys[i - 1], 1), std::wstring_view(&keys[i], 1)) == CSTR_EQUAL) {
            FAIL_CHECK("the keys U+" << std::hex << static_cast<std::uint32_t>(keys[i - 1]) << " and U+"
                                     << static_cast<std::uint32_t>(keys[i]) << " compare equal");
        }
    }

    // Names with surrogate pairs, and letters the tables treat apart.
    const std::vector<std::pair<std::string, std::string>> pairs{
        {"\xF0\x90\x90\xA8.gb", "\xF0\x90\x90\x80.gb"}, // Deseret small and capital long i
        {"\xF0\x9E\xA4\xA2", "\xF0\x9E\xA4\x80"},       // Adlam small and capital alif
        {"\xF0\x9F\x8E\xAE a", "\xF0\x9F\x8E\xAE A"},   // an emoji, then letters that fold
        {"Pok\xC3\xA9mon", "POK\xC3\x89MON"},           // é and É
        {"\xC3\x9F", "\xE1\xBA\x9E"},                   // sharp s and capital sharp s
        {"\xC4\xB1", "I"},                              // dotless i and I
        {"\xCF\x82", "\xCE\xA3"},                       // final sigma and capital sigma
        {"\xE1\x8F\xB8", "\xE1\x8F\xB0"},               // Cherokee small and capital ye
        {"Cafe\xCC\x81", "Caf\xC3\xA9"},                // NFD and NFC
    };
    for (const auto& [a, b] : pairs) {
        INFO(a << " / " << b);
        const std::wstring wideA = rv::core::utf8ToWide(a).value();
        const std::wstring wideB = rv::core::utf8ToWide(b).value();
        CHECK((upper(a) == upper(b)) == (compareIgnoringCase(wideA, wideB) == CSTR_EQUAL));
    }
}

TEST_CASE("core/case_fold: malformed UTF-8 is an error", "[core][case_fold]") {
    for (const std::string_view text : {std::string_view("\xC3"), std::string_view("a\x80"),
                                        std::string_view("\xED\xA0\x80"), std::string_view("\xC0\xAF")}) {
        const auto folded = fileSystemUpperCase(text);
        REQUIRE_FALSE(folded.has_value());
        CHECK(folded.error().code == rv::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("core/case_fold: UTF-16 text keeps its surrogates", "[core][case_fold]") {
    const std::wstring unpaired{L'a', static_cast<wchar_t>(0xD800), L'b', static_cast<wchar_t>(0xDC00)};
    CHECK(upperWide(unpaired) == std::wstring{L'A', static_cast<wchar_t>(0xD800), L'B', static_cast<wchar_t>(0xDC00)});
    const std::wstring paired{L'x', static_cast<wchar_t>(0xD801), static_cast<wchar_t>(0xDC28), L'y'}; // U+10428
    CHECK(upperWide(paired) == std::wstring{L'X', static_cast<wchar_t>(0xD801), static_cast<wchar_t>(0xDC28), L'Y'});
    CHECK(upperWide(std::wstring_view()).empty());
}

TEST_CASE("core/case_fold: the invariant lower case of sort keys", "[core][case_fold]") {
    const auto lower = [](std::string_view utf8) {
        const auto folded = rv::core::invariantLowerCase(utf8);
        REQUIRE(folded.has_value());
        return *folded;
    };
    CHECK(lower("Tiny Tennis (EUROPE) [!] _Z_") == "tiny tennis (europe) [!] _z_");
    CHECK(lower("") == "");
    // The ASCII shortcut agrees with the system call, which the same text with a non-ASCII character goes through.
    std::string ascii;
    for (int c = 1; c < 0x80; ++c) {
        ascii.push_back(static_cast<char>(c));
    }
    const std::string shortcut = lower(ascii);
    const std::string system = lower(ascii + "\xC3\x89"); // É
    REQUIRE(system.size() == shortcut.size() + 2);
    CHECK(system.substr(0, shortcut.size()) == shortcut);
    CHECK(system.substr(shortcut.size()) == "\xC3\xA9"); // é

    // É Ü Ñ; capital sigma and alpha to small sigma (never the final form) and alpha.
    CHECK(lower("CAF\xC3\x89 D\xC3\x9CX \xC3\x91") == "caf\xC3\xA9 d\xC3\xBCx \xC3\xB1");
    CHECK(lower("\xCE\xA3\xCE\x91") == "\xCF\x83\xCE\xB1");
    // One code unit to one, never normalized: NFD stays NFD, sharp s stays; outside the BMP nothing changes (U+10400).
    CHECK(lower("CAFE\xCC\x81") == "cafe\xCC\x81");
    CHECK(lower("STRA\xC3\x9F" "E") == "stra\xC3\x9F" "e");
    CHECK(lower("\xF0\x90\x90\x80 A") == "\xF0\x90\x90\x80 a");
    // Upper and lower case agree on what is a letter pair: folding either way gives one key for é and É.
    CHECK(lower(upper("Pok\xC3\xA9mon")) == lower("Pok\xC3\xA9mon"));

    const auto malformed = rv::core::invariantLowerCase("\xC3");
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("core/case_fold: the casing table has a version", "[core][case_fold]") {
    const std::string version = rv::core::casingTableVersion();
    CHECK_FALSE(version.empty());
    CHECK(version != "unknown");
    CHECK(version.find('.') != std::string::npos);
    CHECK(rv::core::casingTableVersion() == version);
}
