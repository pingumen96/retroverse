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

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>

namespace rv::core {

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
