#pragma once

// Case folding as Windows file systems compare names (ADR-0008 §4): the lookup keys of the catalog and every
// case-insensitive comparison of paths.

#include <string>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::core {

/// Upper-cases UTF-8 text with the casing table of the Windows file systems (NTFS and exFAT compare names that way,
/// as CompareStringOrdinal does when it ignores case): LCMapStringEx with LOCALE_NAME_INVARIANT and LCMAP_UPPERCASE,
/// without linguistic casing, applied to each UTF-16 code unit of the Basic Multilingual Plane; characters outside it
/// (surrogate pairs) never change, as the file systems' tables map code units only. One code unit maps to one, so the
/// text is never normalized (the NFC and NFD forms of a name stay different, as they are different names on disk) and
/// characters without a simple upper case stay as they are (sharp s). Errors: malformed UTF-8 (InvalidArgument), a
/// failure of the system call (PlatformError).
[[nodiscard]] Expected<std::string> fileSystemUpperCase(std::string_view utf8);

/// The same on UTF-16 text, as Windows stores paths; surrogates, paired or not, are kept as they are.
[[nodiscard]] Expected<std::wstring> fileSystemUpperCase(std::wstring_view utf16);

/// Lower-cases UTF-8 text with the same table the other way round: LCMapStringEx with LOCALE_NAME_INVARIANT and
/// LCMAP_LOWERCASE, without linguistic casing, each UTF-16 code unit of the Basic Multilingual Plane to one, characters
/// outside it kept, never normalized. Text to be ordered rather than names to be matched: the sort keys of the catalog
/// (ADR-0008 §5). Errors: malformed UTF-8 (InvalidArgument), a failure of the system call (PlatformError).
[[nodiscard]] Expected<std::string> invariantLowerCase(std::string_view utf8);

/// The version of the casing table fileSystemUpperCase() uses: the NLS version of the invariant locale
/// (GetNLSVersionEx), "<NLS version>.<defined version>" in hexadecimal, or "unknown" when the system does not say.
/// Stored with the lookup keys, which are computed again when it changes (ADR-0008 §4).
[[nodiscard]] std::string casingTableVersion();

} // namespace rv::core
