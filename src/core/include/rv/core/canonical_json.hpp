#pragma once

// Canonical JSON (ADR-0010 §8): one text for one value, so that the same plan gives byte-identical files in Debug and
// Release and its SHA-256 is its digest. Golden texts and digests come from the independent oracle
// `scripts/world_seed_oracle.py` (Python's json and hashlib).
//
// The form:
//   - object members sorted by their keys' code points (the UTF-8 bytes in order; RFC 8785 sorts by UTF-16 code units,
//     which differs only above U+FFFF);
//   - integers in decimal, '-' before a negative one, no fraction or exponent; floating-point numbers are refused (a
//     plan holds millimetres and hundredths of a degree);
//   - strings in UTF-8 with exactly '"', '\' and U+0000 to U+001F escaped: \b \t \n \f \r, the others \u00XX in
//     lowercase hexadecimal (as RFC 8785); '/', DEL and everything above U+007F as they are;
//   - arrays in the order given (the writer of a document sorts what it documents as sorted);
//   - no white space between tokens, except that a top-level array (the document when it is an array, else a member of
//     the document's object) that is not empty holds one element per line: "[\n", the elements separated by ",\n",
//     then "\n]";
//   - the text ends with one line feed.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>

namespace rv::core {

/// Writes a canonical text straight from the values it documents, without a document in between (m3/preview-time:
/// the digests of a whole world built a nlohmann::json document first, most of their time). The text is the one
/// writeCanonicalJson() writes for the document of the same values: the caller gives each object's members in the
/// order of their keys (the form's order: the keys' UTF-8 bytes), and the writer places the commas, the top-level
/// arrays' lines and the final line feed. A mistake of the caller (keys out of order or twice, a value where a key is
/// due, an unbalanced document) or a string that is not well-formed UTF-8 is an error at finish(), with no pointer:
/// a writer whose input may hold such a string falls back to the document's path to name it.
class CanonicalTextWriter {
public:
    /// Appends to `out`.
    explicit CanonicalTextWriter(std::string& out) : m_out(out) {}

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    /// The next member's key (in an object, after the previous member's value).
    void key(std::string_view name);
    void string(std::string_view text);
    void integer(std::int64_t value);
    void unsignedInteger(std::uint64_t value);
    void boolean(bool value);
    void null();
    /// A value given as a document, written as a value nested in this one: every array in it on one line, also an
    /// array given as the document itself or as a member of the document's object, which writeCanonicalJson() writes
    /// one element per line (give such an array with beginArray()). Its levels count from the document's root (the 64
    /// levels of writeCanonicalJson()). Errors: those of writeCanonicalJson(), without the value's pointer.
    void value(const nlohmann::json& value);
    /// A value already written by nestedCanonicalText(), as it is.
    void raw(std::string_view text);
    /// The document ended: its line feed appended. The first error, if any (the text is then partial).
    [[nodiscard]] Status finish();

private:
    struct Frame {
        bool object = false;
        bool lines = false;
        std::size_t count = 0;
        /// An object's last key, and whether its value is due.
        std::string lastKey;
        bool valueDue = false;
    };
    /// Before a value: its separator; false (an error) where no value is due.
    bool beforeValue();
    void fail(std::string message);

    std::string& m_out;
    std::vector<Frame> m_frames;
    bool m_started = false;
    std::optional<Error> m_error;
};

/// The canonical text of `value` written as a value nested in a document (CanonicalTextWriter::raw()): its arrays on
/// one line, no final line feed. Errors: those of writeCanonicalJson().
[[nodiscard]] Expected<std::string> nestedCanonicalText(const nlohmann::json& value);

/// The canonical text of `value`. Errors: InvalidArgument for a floating-point number, a binary value, a discarded
/// value, a string or key that is not well-formed UTF-8 or arrays and objects nested deeper than 64 levels, its context
/// the value's JSON pointer ("/placements/3/x"; "" for the document).
[[nodiscard]] Expected<std::string> writeCanonicalJson(const nlohmann::json& value);

/// The digest of `value`: SHA-256 of its canonical text, as 64 lowercase hexadecimal digits (the plan digest). The
/// digest of a text already written is sha256Hex() of it. Errors: those of writeCanonicalJson().
[[nodiscard]] Expected<std::string> canonicalJsonDigest(const nlohmann::json& value);

/// Reads a canonical text back: strict JSON (no comments), and the text must be exactly the one writeCanonicalJson()
/// writes for the value read, so that a file edited by hand, or written by another writer, is never taken for a
/// canonical one and its digest stays the digest of its bytes. Errors: ParseError (not JSON), ValidationFailed (a
/// value the canonical form refuses, or another text for the value: the byte where it departs is named), with
/// `context` (usually the file).
[[nodiscard]] Expected<nlohmann::json> parseCanonicalJson(std::string_view text, std::string_view context);

} // namespace rv::core
