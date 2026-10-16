#pragma once

// What rv_esde's private XML loader (src/xml.hpp) reads from a file, without pugixml types: the entry point of the
// loader's unit tests and of diagnostics. The ES-DE parsers of the later I2 steps use the loader itself.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::esde {

/// One element as the loader read it.
struct XmlElementOutline {
    std::string name;
    /// In document order, values decoded.
    std::vector<std::pair<std::string, std::string>> attributes;
    /// The character data directly inside the element (its text and CDATA children, concatenated), decoded, with "\n"
    /// line ends.
    std::string text;
    std::vector<XmlElementOutline> children;
};

/// What the loader read from a file.
struct XmlOutline {
    /// The top-level elements in document order.
    std::vector<XmlElementOutline> elements;
    /// What was read leniently (bytes that are not UTF-8, references to no character), one message per kind.
    std::vector<std::string> warnings;
    /// The encoding the file was read in: "UTF-8", "UTF-16LE", "UTF-16BE", "UTF-32LE", "UTF-32BE", "ISO-8859-1".
    std::string encoding;
};

/// What the loader reads from `file` (core::readFileShared up to `maxBytes`, decoded as ES-DE decodes it, every name
/// and value UTF-8, parse_fragment). Errors: those of the loader (ParseError with the line, column and byte offset;
/// NotFound, OutOfRange, ...).
[[nodiscard]] Expected<XmlOutline> outlineXmlFile(const std::filesystem::path& file, std::uintmax_t maxBytes);

} // namespace rv::esde
