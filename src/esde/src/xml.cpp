#include "xml.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/xml_outline.hpp>

namespace rv::esde {
namespace detail {
namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
constexpr char32_t kReplacementCharacter = 0xFFFD;

/// pugixml's defaults (CDATA, line ends, attribute blanks) and fragments, without its decoding of references: the
/// loader decodes them itself (decodeReferences()), as pugixml would but never into malformed UTF-8.
constexpr unsigned kParseOptions = (pugi::parse_default & ~pugi::parse_escapes) | pugi::parse_fragment;

std::string_view nameOf(XmlEncoding encoding) noexcept {
    switch (encoding) {
    case XmlEncoding::Utf8:
        return "UTF-8";
    case XmlEncoding::Utf16Le:
        return "UTF-16LE";
    case XmlEncoding::Utf16Be:
        return "UTF-16BE";
    case XmlEncoding::Utf32Le:
        return "UTF-32LE";
    case XmlEncoding::Utf32Be:
        return "UTF-32BE";
    case XmlEncoding::Latin1:
        return "ISO-8859-1";
    }
    return "UTF-8";
}

bool isBlank(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// A character of an encoding's name as pugixml scans it (its ct_symbol).
bool isNameCharacter(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return byte >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == ':' || c == '-' || c == '.';
}

/// True when the XML declaration that starts `bytes` names ISO-8859-1 or latin1 (any case), read as pugixml's
/// parse_declaration_encoding reads it.
bool declaresLatin1(std::string_view bytes) {
    if (bytes.size() < 6 || !bytes.starts_with("<?xml") || !isBlank(bytes[5])) {
        return false;
    }
    for (std::size_t i = 6; i + 1 < bytes.size(); ++i) {
        if (bytes[i] == '?') {
            return false; // the declaration ends without an encoding
        }
        if (bytes[i] != 'e' || bytes[i + 1] != 'n') {
            continue;
        }
        // The first "en" is "encoding" (the version before it holds none), then blanks, '=', blanks, the name quoted.
        std::size_t at = i;
        if (bytes.substr(at, 8) != "encoding") {
            return false;
        }
        at += 8;
        while (at < bytes.size() && isBlank(bytes[at])) {
            ++at;
        }
        if (at >= bytes.size() || bytes[at] != '=') {
            return false;
        }
        ++at;
        while (at < bytes.size() && isBlank(bytes[at])) {
            ++at;
        }
        const char quote = at < bytes.size() && bytes[at] == '"' ? '"' : '\'';
        if (at >= bytes.size() || bytes[at] != quote) {
            return false;
        }
        const std::size_t start = ++at;
        while (at < bytes.size() && isNameCharacter(bytes[at])) {
            ++at;
        }
        if (at >= bytes.size() || bytes[at] != quote) {
            return false;
        }
        const std::string name = core::toLowerAscii(bytes.substr(start, at - start));
        return name == "iso-8859-1" || name == "latin1";
    }
    return false;
}

struct DetectedEncoding {
    XmlEncoding encoding = XmlEncoding::Utf8;
    std::size_t bomBytes = 0;
};

/// The encoding of a file as pugixml's encoding_auto guesses it (guess_buffer_encoding), which is how ES-DE reads its
/// files: a byte order mark, else the code units of the '<' it starts with, else a declared ISO-8859-1, else UTF-8.
DetectedEncoding detectEncoding(std::string_view bytes) {
    if (bytes.size() < 4) {
        // pugixml guesses nothing from so few bytes: UTF-8, whose byte order mark its parser skips.
        return {XmlEncoding::Utf8, bytes.starts_with(kUtf8Bom) ? kUtf8Bom.size() : 0};
    }
    const auto d0 = static_cast<unsigned char>(bytes[0]);
    const auto d1 = static_cast<unsigned char>(bytes[1]);
    const auto d2 = static_cast<unsigned char>(bytes[2]);
    const auto d3 = static_cast<unsigned char>(bytes[3]);
    if (d0 == 0x00 && d1 == 0x00 && d2 == 0xFE && d3 == 0xFF) {
        return {XmlEncoding::Utf32Be, 4};
    }
    if (d0 == 0xFF && d1 == 0xFE && d2 == 0x00 && d3 == 0x00) {
        return {XmlEncoding::Utf32Le, 4};
    }
    if (d0 == 0xFE && d1 == 0xFF) {
        return {XmlEncoding::Utf16Be, 2};
    }
    if (d0 == 0xFF && d1 == 0xFE) {
        return {XmlEncoding::Utf16Le, 2};
    }
    if (d0 == 0xEF && d1 == 0xBB && d2 == 0xBF) {
        return {XmlEncoding::Utf8, 3};
    }
    if (d0 == 0x00 && d1 == 0x00 && d2 == 0x00 && d3 == 0x3C) {
        return {XmlEncoding::Utf32Be, 0};
    }
    if (d0 == 0x3C && d1 == 0x00 && d2 == 0x00 && d3 == 0x00) {
        return {XmlEncoding::Utf32Le, 0};
    }
    if (d0 == 0x00 && d1 == 0x3C) {
        return {XmlEncoding::Utf16Be, 0};
    }
    if (d0 == 0x3C && d1 == 0x00) {
        return {XmlEncoding::Utf16Le, 0};
    }
    return {declaresLatin1(bytes) ? XmlEncoding::Latin1 : XmlEncoding::Utf8, 0};
}

/// Decodes the character at `pos` of `body` (a file past its byte order mark) into `out` as UTF-8 and returns where
/// the next one starts. `lenient` tells whether the bytes were no character of the encoding: read as Windows-1252 in
/// UTF-8 (as core::utf8WithWindows1252Fallback reads them), as U+FFFD in UTF-16 (an unpaired surrogate, an odd last
/// byte) and UTF-32 (no code point, a short last unit). A declared ISO-8859-1 reads as Windows-1252, as the WHATWG
/// encoding standard reads that label.
std::size_t decodeCharacter(std::string_view body, std::size_t pos, XmlEncoding encoding, std::string& out,
                            bool& lenient) {
    const auto byteAt = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(body[i])); };
    lenient = false;
    switch (encoding) {
    case XmlEncoding::Utf8: {
        const std::uint32_t lead = byteAt(pos);
        const std::size_t length = lead < 0x80                    ? 1
                                   : lead >= 0xC2 && lead <= 0xDF ? 2
                                   : lead >= 0xE0 && lead <= 0xEF ? 3
                                   : lead >= 0xF0 && lead <= 0xF4 ? 4
                                                                  : 0;
        if (length != 0 && length <= body.size() - pos && core::isValidUtf8(body.substr(pos, length))) {
            out.append(body.substr(pos, length));
            return pos + length;
        }
        out += core::windows1252ToUtf8(body.substr(pos, 1));
        lenient = true;
        return pos + 1;
    }
    case XmlEncoding::Latin1:
        out += core::windows1252ToUtf8(body.substr(pos, 1));
        return pos + 1;
    case XmlEncoding::Utf16Le:
    case XmlEncoding::Utf16Be: {
        const bool little = encoding == XmlEncoding::Utf16Le;
        const auto unitAt = [&](std::size_t i) {
            return little ? byteAt(i) | (byteAt(i + 1) << 8) : (byteAt(i) << 8) | byteAt(i + 1);
        };
        if (body.size() - pos < 2) {
            core::appendUtf8(out, kReplacementCharacter);
            lenient = true;
            return body.size();
        }
        const std::uint32_t unit = unitAt(pos);
        if (unit >= 0xD800 && unit <= 0xDBFF && body.size() - pos >= 4) {
            const std::uint32_t next = unitAt(pos + 2);
            if (next >= 0xDC00 && next <= 0xDFFF) {
                core::appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                return pos + 4;
            }
        }
        lenient = unit >= 0xD800 && unit <= 0xDFFF;
        core::appendUtf8(out, lenient ? kReplacementCharacter : unit);
        return pos + 2;
    }
    case XmlEncoding::Utf32Le:
    case XmlEncoding::Utf32Be: {
        if (body.size() - pos < 4) {
            core::appendUtf8(out, kReplacementCharacter);
            lenient = true;
            return body.size();
        }
        const std::uint32_t value =
            encoding == XmlEncoding::Utf32Le
                ? byteAt(pos) | (byteAt(pos + 1) << 8) | (byteAt(pos + 2) << 16) | (byteAt(pos + 3) << 24)
                : (byteAt(pos) << 24) | (byteAt(pos + 1) << 16) | (byteAt(pos + 2) << 8) | byteAt(pos + 3);
        lenient = value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF);
        core::appendUtf8(out, lenient ? kReplacementCharacter : value);
        return pos + 4;
    }
    }
    return body.size();
}

/// `raw`, a text or an attribute value parsed with parse_escapes off, with its references decoded as pugixml's
/// strconv_escape decodes them: the five predefined entities, "&#" decimal digits ";" and "&#x" hexadecimal digits ";"
/// (a lowercase x, as pugixml reads it). A reference to no character (&#0;, a surrogate, beyond U+10FFFF however many
/// digits it has) becomes U+FFFD, counted in `invalid`, where pugixml would write malformed UTF-8 or end the value.
/// Anything else after a '&' (an unknown entity, a malformed reference) stays as written, as pugixml leaves it.
std::string decodeReferences(std::string_view raw, std::size_t& invalid) {
    static constexpr std::array<std::pair<std::string_view, char>, 5> kEntities{{
        {"&amp;", '&'},
        {"&lt;", '<'},
        {"&gt;", '>'},
        {"&quot;", '"'},
        {"&apos;", '\''},
    }};
    std::string out;
    out.reserve(raw.size());
    std::size_t i = 0;
    while (i < raw.size()) {
        const std::size_t ampersand = raw.find('&', i);
        if (ampersand == std::string_view::npos) {
            out.append(raw.substr(i));
            break;
        }
        out.append(raw.substr(i, ampersand - i));
        const std::string_view rest = raw.substr(ampersand);
        std::size_t length = 0; // of the reference at the ampersand, when there is one
        if (rest.starts_with("&#")) {
            const bool hex = rest.size() > 2 && rest[2] == 'x';
            const std::size_t firstDigit = hex ? 3 : 2;
            std::size_t at = firstDigit;
            std::uint32_t value = 0;
            for (; at < rest.size(); ++at) {
                const char c = rest[at];
                const char lower = static_cast<char>(c | 0x20);
                std::uint32_t digit = 0;
                if (c >= '0' && c <= '9') {
                    digit = static_cast<std::uint32_t>(c - '0');
                } else if (hex && lower >= 'a' && lower <= 'f') {
                    digit = static_cast<std::uint32_t>(lower - 'a' + 10);
                } else {
                    break;
                }
                // Past U+10FFFF it names no character, however long it goes on: stop counting there.
                value = std::min<std::uint32_t>(value * (hex ? 16u : 10u) + digit, 0x110000u);
            }
            if (at > firstDigit && at < rest.size() && rest[at] == ';') {
                const bool character = value != 0 && value <= 0x10FFFF && (value < 0xD800 || value > 0xDFFF);
                core::appendUtf8(out, character ? value : kReplacementCharacter);
                if (!character) {
                    ++invalid;
                }
                length = at + 1;
            }
        } else {
            for (const auto& [entity, character] : kEntities) {
                if (rest.starts_with(entity)) {
                    out += character;
                    length = entity.size();
                    break;
                }
            }
        }
        if (length == 0) {
            out += '&';
            length = 1;
        }
        i = ampersand + length;
    }
    return out;
}

bool holdsElement(const pugi::xml_document& document) {
    for (const pugi::xml_node child : document.children()) {
        if (child.type() == pugi::node_element) {
            return true;
        }
    }
    return false;
}

/// What finishTree() stopped at.
struct TreeProblem {
    pugi::xml_node node;
    std::string what;
};

/// Walks the parsed tree once, without recursion: refuses elements nested deeper than kMaxXmlDepth, decodes the
/// references of every text and attribute value (decodeReferences()) and checks that every name and value is UTF-8
/// (it is by construction: the parsed text is, and so is what decodeReferences() writes). `invalidReferences` counts
/// the references to no character, `firstInvalid` is the element of the first.
std::optional<TreeProblem> finishTree(pugi::xml_document& document, std::size_t& invalidReferences,
                                      pugi::xml_node& firstInvalid) {
    const auto decoded = [&](std::string_view raw, pugi::xml_node element) -> std::optional<std::string> {
        if (raw.find('&') == std::string_view::npos) {
            return std::nullopt;
        }
        const std::size_t before = invalidReferences;
        std::string value = decodeReferences(raw, invalidReferences);
        if (invalidReferences != before && !firstInvalid) {
            firstInvalid = element;
        }
        return value;
    };
    std::vector<std::pair<pugi::xml_node, std::size_t>> pending; // a node and its depth, the document's children at 1
    for (pugi::xml_node child = document.last_child(); child; child = child.previous_sibling()) {
        pending.emplace_back(child, 1);
    }
    while (!pending.empty()) {
        auto [node, depth] = pending.back();
        pending.pop_back();
        if (!core::isValidUtf8(node.name())) {
            return TreeProblem{node, "a name that is not UTF-8"};
        }
        switch (node.type()) {
        case pugi::node_element:
            if (depth > kMaxXmlDepth) {
                return TreeProblem{node, fmt::format("elements nested deeper than {} levels", kMaxXmlDepth)};
            }
            for (pugi::xml_attribute attribute : node.attributes()) {
                if (auto value = decoded(attribute.value(), node)) {
                    attribute.set_value(value->c_str(), value->size());
                }
                if (!core::isValidUtf8(attribute.name()) || !core::isValidUtf8(attribute.value())) {
                    return TreeProblem{node, "an attribute that is not UTF-8"};
                }
            }
            for (pugi::xml_node child = node.last_child(); child; child = child.previous_sibling()) {
                pending.emplace_back(child, depth + 1);
            }
            break;
        case pugi::node_pcdata:
            if (auto value = decoded(node.value(), node.parent())) {
                node.set_value(value->c_str(), value->size());
            }
            [[fallthrough]];
        default:
            if (!core::isValidUtf8(node.value())) {
                return TreeProblem{node.parent(), "a text that is not UTF-8"};
            }
            break;
        }
    }
    return std::nullopt;
}

} // namespace

pugi::xml_node XmlDocument::topLevel(std::string_view name) const {
    for (const pugi::xml_node child : m_document.children()) {
        if (child.type() == pugi::node_element && name == child.name()) {
            return child;
        }
    }
    return {};
}

std::size_t XmlDocument::fileOffset(std::size_t textOffset) const {
    if (m_file.empty()) {
        return m_bomBytes + textOffset; // the text is the file's bytes past its byte order mark
    }
    // Decoded again up to the character that holds the offset.
    const std::string_view body = std::string_view(m_file).substr(m_bomBytes);
    std::string character;
    std::size_t pos = 0;
    std::size_t decoded = 0;
    while (pos < body.size()) {
        character.clear();
        bool lenient = false;
        const std::size_t next = decodeCharacter(body, pos, m_encoding, character, lenient);
        if (decoded + character.size() > textOffset) {
            break;
        }
        decoded += character.size();
        pos = next;
    }
    return m_bomBytes + pos;
}

std::string XmlDocument::locate(std::size_t textOffset) const {
    textOffset = std::min(textOffset, m_text.size());
    std::size_t line = 1;
    std::size_t lineStart = 0;
    for (std::size_t i = 0; i < textOffset; ++i) {
        if (m_text[i] == '\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    std::size_t column = 1;
    for (std::size_t i = lineStart; i < textOffset; ++i) {
        if ((static_cast<unsigned char>(m_text[i]) & 0xC0u) != 0x80u) {
            ++column; // a character starts here (UTF-8 continuation bytes do not count)
        }
    }
    return fmt::format("line {}, column {} (byte offset {})", line, column, fileOffset(textOffset));
}

std::string XmlDocument::locate(pugi::xml_node node) const {
    const std::ptrdiff_t offset = node.offset_debug();
    if (offset < 0) {
        return "an unknown place";
    }
    return locate(static_cast<std::size_t>(offset));
}

Expected<XmlDocument> parseXml(std::string bytes, std::string source) {
    XmlDocument document;
    const DetectedEncoding detected = detectEncoding(bytes);
    document.m_encoding = detected.encoding;
    document.m_bomBytes = detected.bomBytes;
    std::size_t lenient = 0;      // characters read leniently while decoding
    std::size_t firstLenient = 0; // the text offset of the first
    const auto invalidUtf8 = detected.encoding == XmlEncoding::Utf8
                                 ? core::findInvalidUtf8(std::string_view(bytes).substr(detected.bomBytes))
                                 : std::nullopt;
    if (detected.encoding == XmlEncoding::Utf8 && !invalidUtf8) {
        bytes.erase(0, detected.bomBytes); // the common case: the text is the file
        document.m_text = std::move(bytes);
    } else if (detected.encoding == XmlEncoding::Utf8) {
        document.m_text =
            core::utf8WithWindows1252Fallback(std::string_view(bytes).substr(detected.bomBytes), &lenient);
        firstLenient = *invalidUtf8; // what comes before it is copied as it is
        document.m_file = std::move(bytes);
    } else {
        const std::string_view body = std::string_view(bytes).substr(detected.bomBytes);
        document.m_text.reserve(body.size());
        std::size_t pos = 0;
        while (pos < body.size()) {
            const std::size_t at = document.m_text.size();
            bool replaced = false;
            pos = decodeCharacter(body, pos, detected.encoding, document.m_text, replaced);
            if (replaced && lenient++ == 0) {
                firstLenient = at;
            }
        }
        document.m_file = std::move(bytes);
    }

    const std::string& text = document.m_text;
    const auto fail = [&](std::size_t textOffset, std::string_view what) {
        return makeError(ErrorCode::ParseError, fmt::format("{} at {}", what, document.locate(textOffset)), source);
    };
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return makeError(ErrorCode::ParseError, "the file is empty", source);
    }
    // XML has no NUL character, and pugixml would take one for the end of the text: a file whose end a crash filled
    // with zeros would pass for a shorter one.
    if (const std::size_t nul = text.find('\0'); nul != std::string::npos) {
        return fail(nul, "a NUL character");
    }

    const pugi::xml_parse_result result =
        document.m_document.load_buffer(text.data(), text.size(), kParseOptions, pugi::encoding_utf8);
    if (!result) {
        return fail(static_cast<std::size_t>(std::max<std::ptrdiff_t>(result.offset, 0)), result.description());
    }
    std::size_t invalidReferences = 0;
    pugi::xml_node firstInvalid;
    if (const auto problem = finishTree(document.m_document, invalidReferences, firstInvalid)) {
        return makeError(ErrorCode::ParseError,
                         fmt::format("{} at {}", problem->what, document.locate(problem->node)), source);
    }
    if (!holdsElement(document.m_document)) {
        return makeError(ErrorCode::ParseError, "no XML element in the file", source);
    }

    if (lenient > 0) {
        document.m_warnings.push_back(
            detected.encoding == XmlEncoding::Utf8
                ? fmt::format("{} byte(s) that are not UTF-8 read as Windows-1252 characters, the first at {}",
                              lenient, document.locate(firstLenient))
                : fmt::format("{} invalid {} code unit(s) read as U+FFFD, the first at {}", lenient,
                              nameOf(detected.encoding), document.locate(firstLenient)));
    }
    if (invalidReferences > 0) {
        document.m_warnings.push_back(fmt::format(
            "{} character reference(s) to no character (&#0;, a surrogate, beyond U+10FFFF) read as U+FFFD, the "
            "first in the element at {}",
            invalidReferences, document.locate(firstInvalid)));
    }
    document.m_source = std::move(source);
    return document;
}

Expected<XmlDocument> loadXmlFile(const std::filesystem::path& file, std::uintmax_t maxBytes) {
    auto bytes = core::readFileShared(file, maxBytes);
    if (!bytes) {
        return propagate(std::move(bytes.error()));
    }
    auto document = parseXml(std::move(*bytes), core::pathToUtf8Lossy(file));
    if (document) {
        for (const std::string& warning : document->warnings()) {
            spdlog::warn("{}: {}", document->source(), warning);
        }
    }
    return document;
}

} // namespace detail

namespace {

/// An element's outline; recursion is bounded by the loader's kMaxXmlDepth.
XmlElementOutline outlineOf(const pugi::xml_node element) {
    XmlElementOutline outline;
    outline.name = element.name();
    for (const pugi::xml_attribute attribute : element.attributes()) {
        outline.attributes.emplace_back(attribute.name(), attribute.value());
    }
    for (const pugi::xml_node child : element.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            outline.text += child.value();
        } else if (child.type() == pugi::node_element) {
            outline.children.push_back(outlineOf(child));
        }
    }
    return outline;
}

} // namespace

Expected<XmlOutline> outlineXmlFile(const std::filesystem::path& file, std::uintmax_t maxBytes) {
    auto document = detail::loadXmlFile(file, maxBytes);
    if (!document) {
        return propagate(std::move(document.error()));
    }
    XmlOutline outline;
    outline.encoding = std::string(detail::nameOf(document->encoding()));
    outline.warnings = document->warnings();
    for (const pugi::xml_node child : document->document().children()) {
        if (child.type() == pugi::node_element) {
            outline.elements.push_back(outlineOf(child));
        }
    }
    return outline;
}

} // namespace rv::esde
