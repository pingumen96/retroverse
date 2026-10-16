// rv_esde's XML loader through its test entry point (rv/esde/xml_outline.hpp): ES-DE's file shapes (fragments with
// several top-level elements), the encodings ES-DE's pugixml reads, line ends, what is read leniently (bytes that are
// not UTF-8, references to no character) and the ways a file can be broken. The files are written at test time:
// .gitattributes would turn a committed CRLF or BOM fixture into LF.

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <string>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/xml_outline.hpp>

#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::esde::outlineXmlFile;
using rv::esde::XmlElementOutline;
using rv::esde::XmlOutline;

namespace {

constexpr std::uintmax_t kLimit = 1024 * 1024;

fs::path written(const rv::test::TempDir& dir, std::string_view name, std::string_view content) {
    const fs::path file = dir.file(name);
    REQUIRE(rv::core::writeTextFileAtomic(file, content).has_value());
    return file;
}

XmlOutline outline(const fs::path& file, std::uintmax_t limit = kLimit) {
    auto read = outlineXmlFile(file, limit);
    INFO((read ? std::string("read") : read.error().toString()));
    REQUIRE(read.has_value());
    return std::move(*read);
}

rv::Error failure(const fs::path& file, std::uintmax_t limit = kLimit) {
    const auto read = outlineXmlFile(file, limit);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().context == rv::core::pathToUtf8(file));
    return read.error();
}

/// The number after "byte offset " in a message.
std::size_t byteOffsetIn(const std::string& message) {
    const std::size_t at = message.find("byte offset ");
    REQUIRE(at != std::string::npos);
    std::size_t value = 0;
    const char* begin = message.data() + at + 12;
    REQUIRE(std::from_chars(begin, message.data() + message.size(), value).ec == std::errc());
    return value;
}

/// `text` in UTF-16, little or big endian, with or without its byte order mark.
std::string utf16(std::u32string_view text, bool little, bool bom) {
    std::string out;
    const auto put = [&](std::uint32_t unit) {
        const char low = static_cast<char>(unit & 0xFF);
        const char high = static_cast<char>((unit >> 8) & 0xFF);
        out += little ? low : high;
        out += little ? high : low;
    };
    if (bom) {
        put(0xFEFF);
    }
    for (const char32_t c : text) {
        if (c >= 0x10000) {
            put(0xD800 + ((c - 0x10000) >> 10));
            put(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
            put(c);
        }
    }
    return out;
}

/// `text` in UTF-32, little or big endian, with or without its byte order mark.
std::string utf32(std::u32string_view text, bool little, bool bom) {
    std::string out;
    const auto put = [&](std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out += static_cast<char>((value >> (little ? 8 * i : 8 * (3 - i))) & 0xFF);
        }
    };
    if (bom) {
        put(0xFEFF);
    }
    for (const char32_t c : text) {
        put(c);
    }
    return out;
}

void checkAllUtf8(const XmlElementOutline& element) {
    CHECK(rv::core::isValidUtf8(element.name));
    CHECK(rv::core::isValidUtf8(element.text));
    for (const auto& [name, value] : element.attributes) {
        CHECK(rv::core::isValidUtf8(name));
        CHECK(rv::core::isValidUtf8(value));
    }
    for (const XmlElementOutline& child : element.children) {
        checkAllUtf8(child);
    }
}

} // namespace

TEST_CASE("esde/xml: a gamelist with an alternative emulator beside the game list", "[esde][xml]") {
    rv::test::TempDir dir;
    const auto file = written(dir, "gamelist.xml",
                              "<?xml version=\"1.0\"?>\n"
                              "<alternativeEmulator>\n"
                              "\t<label>Snes9x - Current</label>\n"
                              "</alternativeEmulator>\n"
                              "<gameList>\n"
                              "\t<game>\n"
                              "\t\t<path>./Alpha Quest (World).sfc</path>\n"
                              "\t\t<name>Alpha &amp; Omega &gt; Beta</name>\n"
                              "\t</game>\n"
                              "\t<folder>\n"
                              "\t\t<path>./S</path>\n"
                              "\t</folder>\n"
                              "</gameList>\n");
    const XmlOutline read = outline(file);
    CHECK(read.encoding == "UTF-8");
    CHECK(read.warnings.empty());
    const auto& elements = read.elements;
    REQUIRE(elements.size() == 2);
    CHECK(elements[0].name == "alternativeEmulator");
    REQUIRE(elements[0].children.size() == 1);
    CHECK(elements[0].children[0].text == "Snes9x - Current");
    CHECK(elements[1].name == "gameList");
    REQUIRE(elements[1].children.size() == 2);
    const XmlElementOutline& game = elements[1].children[0];
    CHECK(game.name == "game");
    REQUIRE(game.children.size() == 2);
    CHECK(game.children[0].text == "./Alpha Quest (World).sfc");
    CHECK(game.children[1].text == "Alpha & Omega > Beta");
    CHECK(elements[1].children[1].name == "folder");
}

TEST_CASE("esde/xml: settings without a root element", "[esde][xml]") {
    rv::test::TempDir dir;
    const auto file = written(dir, "es_settings.xml",
                              "<?xml version=\"1.0\"?>\n"
                              "<bool name=\"ShowHiddenFiles\" value=\"true\" />\n"
                              "<int name=\"MaxPlayTimeTracking\" value=\"8\" />\n"
                              "<string name=\"ROMDirectory\" value=\"D:\\Roms\\Emulators\" />\n"
                              "<string name=\"MediaDirectory\" value=\"\" />\n");
    const auto elements = outline(file).elements;
    REQUIRE(elements.size() == 4);
    CHECK(elements[0].name == "bool");
    CHECK(elements[0].attributes ==
          std::vector<std::pair<std::string, std::string>>{{"name", "ShowHiddenFiles"}, {"value", "true"}});
    CHECK(elements[2].attributes[1].second == "D:\\Roms\\Emulators");
    CHECK(elements[3].attributes[1].second.empty());
}

TEST_CASE("esde/xml: a UTF-8 BOM and CRLF line ends", "[esde][xml]") {
    rv::test::TempDir dir;
    const auto file = written(dir, "crlf.xml",
                              "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                              "<gameList>\r\n"
                              "\t<game>\r\n"
                              "\t\t<name>Pok\xC3\xA9mon Party Mini</name>\r\n"
                              "\t\t<desc>Line one.\r\nLine two.</desc>\r\n"
                              "\t\t<developer attr=\"a\r\nb\">Dev</developer>\r\n"
                              "\t</game>\r\n"
                              "</gameList>\r\n");
    const XmlOutline read = outline(file);
    CHECK(read.warnings.empty());
    REQUIRE(read.elements.size() == 1);
    const XmlElementOutline& game = read.elements[0].children.at(0);
    CHECK(game.children.at(0).text == "Pok\xC3\xA9mon Party Mini");
    CHECK(game.children.at(1).text == "Line one.\nLine two.");
    CHECK(game.children.at(2).attributes.at(0).second == "a b"); // attribute line ends become spaces (XML 1.0)
}

TEST_CASE("esde/xml: entities and character references", "[esde][xml]") {
    rv::test::TempDir dir;
    const auto file = written(dir, "entities.xml",
                              "<gameList><game>"
                              "<name>Ball &amp; Chain</name>"
                              "<desc>Ball &amp;amp; Chain, 1 &gt; 0, &#233;t&#xE9; &#x1F3AE;&#65;&#x42;</desc>"
                              "<publisher><![CDATA[A & <B> &#0;]]></publisher>"
                              "<genre>&#X41; &#; &#x; &#12a; &amp &unknown; &lt;&quot;&apos;</genre>"
                              "<lines>&#13;&#10;</lines>"
                              "<rating value=\"&#9;tab&#x9;\" raw=\"a\tb\" quoted=\"&quot;q&quot; &#x26;\"/>"
                              "</game></gameList>");
    const XmlOutline read = outline(file);
    CHECK(read.warnings.empty());
    const XmlElementOutline& game = read.elements.at(0).children.at(0);
    CHECK(game.children.at(0).text == "Ball & Chain");
    CHECK(game.children.at(1).text ==
          "Ball &amp; Chain, 1 > 0, \xC3\xA9t\xC3\xA9 \xF0\x9F\x8E\xAE" "AB"); // double-escaped stays once
    CHECK(game.children.at(2).text == "A & <B> &#0;");                        // CDATA is never decoded
    // What pugixml leaves as written: an uppercase X, references without digits or with others, unknown entities.
    CHECK(game.children.at(3).text == "&#X41; &#; &#x; &#12a; &amp &unknown; <\"'");
    CHECK(game.children.at(4).text == "\r\n"); // decoded line ends are not normalized
    const auto& attributes = game.children.at(5).attributes;
    CHECK(attributes.at(0).second == "\ttab\t"); // decoded tabs stay tabs...
    CHECK(attributes.at(1).second == "a b");     // ...written ones become spaces
    CHECK(attributes.at(2).second == "\"q\" &");

    // A DOCTYPE's entities are never expanded: nothing is defined, nothing external is fetched.
    const auto doctype = written(dir, "doctype.xml",
                                 "<!DOCTYPE x [<!ENTITY boom \"expanded\"> <!ENTITY ext SYSTEM \"file:///C:/x\">]>"
                                 "<x>&boom;&ext;</x>");
    const auto unexpanded = outline(doctype).elements;
    REQUIRE(unexpanded.size() == 1);
    CHECK(unexpanded[0].text == "&boom;&ext;");
}

TEST_CASE("esde/xml: references to no character are read as U+FFFD, never as malformed UTF-8", "[esde][xml]") {
    rv::test::TempDir dir;
    const auto file = written(dir, "references.xml",
                              "<gameList>\n"
                              "<a b=\"q&#xDFFF;r\">x&#xD800;y</a>\n"
                              "<c>x&#x110000;y</c>\n"
                              "<d>x&#xFFFFFFFF;y</d>\n"
                              "<e>x&#99999999999999999999;y</e>\n"
                              "<f>x&#0;y and the rest</f>\n"
                              "<g>&#x0000;&#x10FFFF;&#xFFFD;</g>\n"
                              "</gameList>\n");
    const XmlOutline read = outline(file);
    const auto& children = read.elements.at(0).children;
    REQUIRE(children.size() == 6);
    const std::string replacement = "\xEF\xBF\xBD";
    CHECK(children[0].text == "x" + replacement + "y");
    CHECK(children[0].attributes.at(0).second == "q" + replacement + "r");
    CHECK(children[1].text == "x" + replacement + "y");
    CHECK(children[2].text == "x" + replacement + "y");
    CHECK(children[3].text == "x" + replacement + "y");
    CHECK(children[4].text == "x" + replacement + "y and the rest"); // never cut short
    CHECK(children[5].text == replacement + "\xF4\x8F\xBF\xBF" + replacement);
    checkAllUtf8(read.elements.at(0));
    REQUIRE(read.warnings.size() == 1);
    CHECK(read.warnings[0].find("7 character reference(s) to no character") != std::string::npos);
    CHECK(read.warnings[0].find("line 2") != std::string::npos); // the element of the first
}

TEST_CASE("esde/xml: bytes that are not UTF-8 are read as Windows-1252", "[esde][xml]") {
    rv::test::TempDir dir;
    // An ANSI editor's bytes in a UTF-8 file: "Caf" then a Windows-1252 é at byte offset 25, later curly quotes.
    const auto file =
        written(dir, "latin1.xml",
                "<gameList><game><name>Caf\xE9</name><desc>Pok\xC3\xA9mon \x93quoted\x94</desc></game></gameList>");
    const XmlOutline read = outline(file);
    CHECK(read.encoding == "UTF-8");
    const XmlElementOutline& game = read.elements.at(0).children.at(0);
    CHECK(game.children.at(0).text == "Caf\xC3\xA9");
    CHECK(game.children.at(1).text == "Pok\xC3\xA9mon \xE2\x80\x9Cquoted\xE2\x80\x9D");
    REQUIRE(read.warnings.size() == 1);
    CHECK(read.warnings[0].find("3 byte(s) that are not UTF-8") != std::string::npos);
    CHECK(read.warnings[0].find("line 1, column 26 (byte offset 25)") != std::string::npos);

    // An error after such bytes is placed in the file: three é read as Windows-1252 shift the text by 3 bytes.
    const rv::Error utf8 = failure(written(dir, "utf8.xml", "<a>\xC3\xA9\xC3\xA9\xC3\xA9</b>"));
    const rv::Error ansi = failure(written(dir, "ansi.xml", "<a>\xE9\xE9\xE9</b>"));
    CHECK(byteOffsetIn(ansi.message) + 3 == byteOffsetIn(utf8.message));
}

TEST_CASE("esde/xml: the encodings ES-DE's pugixml reads", "[esde][xml]") {
    rv::test::TempDir dir;
    const std::u32string_view text = U"<?xml version=\"1.0\"?>\r\n<gameList>\r\n"
                                     U"<game><name>Pok\u00E9mon \U0001F3AE</name></game>\r\n</gameList>\r\n";
    const std::string name = "Pok\xC3\xA9mon \xF0\x9F\x8E\xAE";
    const auto check = [&](std::string_view fileName, const std::string& bytes, std::string_view encoding) {
        INFO(fileName);
        const XmlOutline read = outline(written(dir, fileName, bytes));
        CHECK(read.encoding == encoding);
        CHECK(read.warnings.empty());
        REQUIRE(read.elements.size() == 1);
        CHECK(read.elements[0].children.at(0).children.at(0).text == name);
    };
    check("utf16le-bom.xml", utf16(text, true, true), "UTF-16LE");
    check("utf16be-bom.xml", utf16(text, false, true), "UTF-16BE");
    check("utf16le.xml", utf16(text, true, false), "UTF-16LE"); // told by the code units of '<'
    check("utf16be.xml", utf16(text, false, false), "UTF-16BE");
    check("utf32le-bom.xml", utf32(text, true, true), "UTF-32LE");
    check("utf32be.xml", utf32(text, false, false), "UTF-32BE");

    // A declared ISO-8859-1 or latin1 (read as Windows-1252: 0x80 is the euro sign).
    for (const std::string_view declaration :
         {std::string_view("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>"),
          std::string_view("<?xml version='1.0' encoding = 'LATIN1'?>")}) {
        const XmlOutline read = outline(written(
            dir, "latin1.xml", std::string(declaration) + "<gameList><name>Caf\xE9 \x80 \xC3\xA9</name></gameList>"));
        CHECK(read.encoding == "ISO-8859-1");
        CHECK(read.warnings.empty());
        CHECK(read.elements.at(0).children.at(0).text == "Caf\xC3\xA9 \xE2\x82\xAC \xC3\x83\xC2\xA9");
    }

    // A UTF-16 unit that is no character: U+FFFD, reported.
    const std::string broken = utf16(U"<a>x", true, true) + std::string("\x00\xD8", 2) + utf16(U"y</a>", true, false);
    const XmlOutline read = outline(written(dir, "unpaired.xml", broken));
    CHECK(read.elements.at(0).text == "x\xEF\xBF\xBDy");
    REQUIRE(read.warnings.size() == 1);
    CHECK(read.warnings[0].find("1 invalid UTF-16LE code unit(s)") != std::string::npos);

    // An error in a UTF-16 file is placed in the file: two bytes per character after the byte order mark.
    const std::u32string_view mismatched = U"<gameList>\n<game>\n</gameList>\n";
    const rv::Error utf8 = failure(written(dir, "mismatch8.xml", "<gameList>\n<game>\n</gameList>\n"));
    const rv::Error wide = failure(written(dir, "mismatch16.xml", utf16(mismatched, true, true)));
    CHECK(wide.message.find("line 3") != std::string::npos);
    CHECK(byteOffsetIn(wide.message) == 2 + 2 * byteOffsetIn(utf8.message));
}

TEST_CASE("esde/xml: broken files are errors with the file and the place", "[esde][xml]") {
    rv::test::TempDir dir;

    SECTION("truncated, as when ES-DE is writing it") {
        const auto file = written(dir, "truncated.xml",
                                  "<?xml version=\"1.0\"?>\n<gameList>\n\t<game>\n\t\t<path>./a.sfc</path>\n");
        const rv::Error error = failure(file);
        CHECK(error.code == rv::ErrorCode::ParseError);
        CHECK(error.message.find("line ") != std::string::npos);
        CHECK(error.message.find("byte offset ") != std::string::npos);
    }
    SECTION("cut inside a tag") {
        const rv::Error error = failure(written(dir, "cut.xml", "<gameList><game><pa"));
        CHECK(error.code == rv::ErrorCode::ParseError);
    }
    SECTION("mismatched tags, at their place") {
        const rv::Error error = failure(written(dir, "mismatch.xml", "<gameList>\n<game>\n</gameList>\n"));
        CHECK(error.code == rv::ErrorCode::ParseError);
        CHECK(error.message.find("line 3") != std::string::npos);
    }
    SECTION("empty, blank, a byte order mark alone") {
        for (const std::string_view content :
             {std::string_view(), std::string_view(" \r\n\t\n"), std::string_view("\xEF\xBB\xBF"),
              std::string_view("\xFF\xFE\x20\x00\x0A\x00", 6)}) {
            const rv::Error error = failure(written(dir, "empty.xml", content));
            CHECK(error.code == rv::ErrorCode::ParseError);
            CHECK(error.message.find("empty") != std::string::npos);
        }
    }
    SECTION("no element at all") {
        const rv::Error error = failure(written(dir, "none.xml", "<?xml version=\"1.0\"?>\n<!-- nothing -->\n"));
        CHECK(error.message.find("no XML element") != std::string::npos);
    }
    SECTION("a NUL character, as a crash leaves at the end of a file") {
        const rv::Error error = failure(written(dir, "zeros.xml", std::string_view("<gameList/>\n\0\0\0\0", 16)));
        CHECK(error.message.find("NUL") != std::string::npos);
        CHECK(error.message.find("byte offset 12") != std::string::npos);
        const rv::Error wide =
            failure(written(dir, "zeros16.xml", utf16(U"<gameList/>", true, true) + std::string(4, '\0')));
        CHECK(wide.message.find("NUL") != std::string::npos);
        CHECK(wide.message.find("byte offset 24") != std::string::npos);
    }
}

TEST_CASE("esde/xml: elements nested too deep are an error, never a crash", "[esde][xml]") {
    rv::test::TempDir dir;
    const auto nested = [](std::size_t levels) {
        std::string text;
        for (std::size_t i = 0; i < levels; ++i) {
            text += "<a>";
        }
        text += "x";
        for (std::size_t i = 0; i < levels; ++i) {
            text += "</a>";
        }
        return text;
    };
    const XmlOutline deepest = outline(written(dir, "64.xml", nested(64)));
    const XmlElementOutline* element = &deepest.elements.at(0);
    for (int level = 1; level < 64; ++level) {
        REQUIRE(element->children.size() == 1);
        element = &element->children[0];
    }
    CHECK(element->text == "x");

    const rv::Error error = failure(written(dir, "65.xml", nested(65)));
    CHECK(error.code == rv::ErrorCode::ParseError);
    CHECK(error.message.find("nested deeper than 64 levels") != std::string::npos);
    CHECK(error.message.find("byte offset 193") != std::string::npos); // the 65th <a>'s name
    const rv::Error hostile = failure(written(dir, "100000.xml", nested(100000)));
    CHECK(hostile.message.find("nested deeper than 64 levels") != std::string::npos);
}

TEST_CASE("esde/xml: the size cap and a missing file", "[esde][xml]") {
    rv::test::TempDir dir;
    const auto file = written(dir, "big.xml", "<gameList>" + std::string(4096, ' ') + "</gameList>");
    CHECK(outline(file, 8192).elements.size() == 1);
    CHECK(failure(file, 4096).code == rv::ErrorCode::OutOfRange);
    CHECK(failure(dir.file("missing.xml")).code == rv::ErrorCode::NotFound);
}
