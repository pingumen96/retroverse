#pragma once

// The XML loader of rv_esde, private to the module: pugixml never appears in a public header (ADR-0008 §2). ES-DE's
// files are read through rv::core::readFileShared (ES-DE may be rewriting them; it is never blocked), decoded the way
// ES-DE's pugixml decodes them (its encoding_auto: UTF-8, UTF-16 and UTF-32 by their byte order mark or the '<' they
// start with, ISO-8859-1 when the declaration says so), and parsed with parse_default | parse_fragment: es_settings.xml
// has no root element and a gamelist may hold an <alternativeEmulator> beside its <gameList>. What ES-DE reads
// leniently is read here too, so that both frontends see the same systems and games (ADR-0008 §3), and reported
// (XmlDocument::warnings()): bytes that are not UTF-8 are read as Windows-1252 characters, character references to no
// character (&#0;, surrogates, beyond U+10FFFF) as U+FFFD. Every name and value of the document is UTF-8. A file that
// is cut short, padded with NUL characters or nested absurdly deep is an error (it is being written, or broken). No
// XPath (it throws) and no DOCTYPE processing (no entities but the five predefined ones and character references,
// nothing external).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <pugixml.hpp>

#include <rv/core/expected.hpp>

namespace rv::esde::detail {

/// The largest XML file read unless the caller says otherwise: ES-DE's largest files are a few MiB (a gamelist of
/// 19,000 games, about 7 MiB).
inline constexpr std::uintmax_t kDefaultXmlLimitBytes = 64ull * 1024ull * 1024ull;

/// Elements nested deeper than this are an error: ES-DE's files nest four levels at most, so the parsers may walk a
/// document recursively.
inline constexpr std::size_t kMaxXmlDepth = 64;

/// The encodings the loader reads, as pugixml's encoding_auto tells them apart.
enum class XmlEncoding { Utf8, Utf16Le, Utf16Be, Utf32Le, Utf32Be, Latin1 };

/// A parsed XML file: its document, whose children are the file's top-level nodes, and its source for messages.
class XmlDocument {
public:
    [[nodiscard]] const pugi::xml_document& document() const noexcept { return m_document; }
    /// The first top-level element named `name`, or an empty node.
    [[nodiscard]] pugi::xml_node topLevel(std::string_view name) const;
    /// The file (UTF-8) or the name parseXml() was given.
    [[nodiscard]] const std::string& source() const noexcept { return m_source; }
    /// The encoding the file was read in.
    [[nodiscard]] XmlEncoding encoding() const noexcept { return m_encoding; }
    /// What was read leniently, one message per kind with the number of occurrences and the place of the first; empty
    /// for a clean file. loadXmlFile() logs them; the importer reports them as issues of the file.
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return m_warnings; }
    /// "line L, column C (byte offset N)" of a byte offset of the UTF-8 text the document was parsed from (the file
    /// decoded, its byte order mark left out), for messages: the line and the column (in characters) as an editor
    /// shows them, the byte offset in the file itself.
    [[nodiscard]] std::string locate(std::size_t textOffset) const;
    /// locate() of where a node starts ("an unknown place" for a text node whose value the loader decoded).
    [[nodiscard]] std::string locate(pugi::xml_node node) const;

private:
    friend Expected<XmlDocument> parseXml(std::string bytes, std::string source);

    /// The byte offset in the file of a byte offset of m_text.
    [[nodiscard]] std::size_t fileOffset(std::size_t textOffset) const;

    pugi::xml_document m_document;
    std::string m_source;
    std::string m_text; ///< the UTF-8 text parsed: lines and columns of messages
    /// The file as read, kept when m_text is not its bytes past the byte order mark (another encoding, bytes read as
    /// Windows-1252): byte offsets of messages.
    std::string m_file;
    XmlEncoding m_encoding = XmlEncoding::Utf8;
    std::size_t m_bomBytes = 0;
    std::vector<std::string> m_warnings;
};

/// Loads `file`: read with core::readFileShared up to `maxBytes`, then parseXml(); the warnings are logged. Errors
/// carry the file as their context: those of reading (NotFound, Unavailable, Conflict, OutOfRange, IoError) and those
/// of parseXml().
[[nodiscard]] Expected<XmlDocument> loadXmlFile(const std::filesystem::path& file,
                                                std::uintmax_t maxBytes = kDefaultXmlLimitBytes);

/// Parses the bytes of an XML file; `source` names it in messages. Decoded as described above (a byte order mark
/// skipped, a declared ISO-8859-1 read as Windows-1252 as the WHATWG encoding standard reads it); line ends become
/// "\n"; the predefined entities and character references are decoded as pugixml decodes them, unknown entities and
/// malformed references kept as written. Errors (ParseError, `source` as context, the place in the message): an empty
/// text (nothing but blanks), a NUL character, what pugixml refuses (a truncated file among it), elements nested
/// deeper than kMaxXmlDepth, a text without any element. Which top-level elements must be there is the caller's
/// business.
[[nodiscard]] Expected<XmlDocument> parseXml(std::string bytes, std::string source);

} // namespace rv::esde::detail
