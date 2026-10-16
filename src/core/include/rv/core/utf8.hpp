#pragma once

// UTF-8 validation and conversions to and from UTF-16 (Windows' wide strings, std::filesystem::path), without
// exceptions: names listed from a file system can hold unpaired UTF-16 surrogates and files can hold malformed UTF-8,
// and neither may escape as an exception (rv::core::pathFromUtf8 and pathToUtf8 throw on such text).

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::core {

/// Byte offset of the first malformed sequence of `text`, or nothing when it is well-formed UTF-8 (Unicode table 3-7):
/// a continuation byte without a lead byte, a truncated sequence, an overlong form, a UTF-16 surrogate, a code point
/// above U+10FFFF and the bytes C0, C1 and F5 to FF are malformed. NUL bytes are well-formed.
[[nodiscard]] std::optional<std::size_t> findInvalidUtf8(std::string_view text) noexcept;

/// True when `text` is well-formed UTF-8 (findInvalidUtf8() finds nothing).
[[nodiscard]] bool isValidUtf8(std::string_view text) noexcept;

/// Appends the UTF-8 form of `codePoint` to `out`; what is no Unicode scalar value (a surrogate, beyond U+10FFFF)
/// appends U+FFFD, so that `out` stays well-formed.
void appendUtf8(std::string& out, char32_t codePoint);

/// UTF-8 to UTF-16. Malformed UTF-8 is an error (InvalidArgument, naming its byte offset), never replaced.
[[nodiscard]] Expected<std::wstring> utf8ToWide(std::string_view utf8);

/// UTF-16 to UTF-8. An unpaired surrogate is an error (InvalidArgument, naming its code unit offset), never replaced.
[[nodiscard]] Expected<std::string> wideToUtf8(std::wstring_view wide);

/// wideToUtf8() with every unpaired surrogate replaced by U+FFFD: for messages, logs and names shown to people, never
/// for a path that is opened again.
[[nodiscard]] std::string wideToUtf8Lossy(std::wstring_view wide);

/// pathFromUtf8() without exceptions: malformed UTF-8 is an error (InvalidArgument).
[[nodiscard]] Expected<std::filesystem::path> tryPathFromUtf8(std::string_view utf8);

/// pathToUtf8() without exceptions: a path holding an unpaired surrogate is an error (InvalidArgument).
[[nodiscard]] Expected<std::string> tryPathToUtf8(const std::filesystem::path& path);

/// pathToUtf8() for messages and logs: unpaired surrogates become U+FFFD. Never throws.
[[nodiscard]] std::string pathToUtf8Lossy(const std::filesystem::path& path);

/// Windows-1252 text (the ANSI code page of Western Windows) as UTF-8, every byte one character; the five bytes
/// Windows-1252 leaves undefined (81, 8D, 8F, 90, 9D) read as the C1 controls of the same value, as Windows reads them.
[[nodiscard]] std::string windows1252ToUtf8(std::string_view bytes);

/// Text that should be UTF-8 but may not be (files that other programs or people wrote, ADR-0008 §3): every
/// well-formed UTF-8 sequence is kept, every byte that starts none is read as its Windows-1252 character
/// ("Caf\xE9" reads "Café"), so that the result is always UTF-8 and a file saved as ANSI reads as intended. The number
/// of bytes read as Windows-1252 goes to `fallbackBytes` when given.
[[nodiscard]] std::string utf8WithWindows1252Fallback(std::string_view bytes, std::size_t* fallbackBytes = nullptr);

} // namespace rv::core
