#pragma once

// Paths read from files (an ES-DE gamelist's <path>) resolved against the folder they belong to, without ever leaving
// it (specification §19.1: directory traversal).

#include <filesystem>
#include <string>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::core {

/// Resolves `pathText`, a path read from a file such as an ES-DE gamelist, against `base`, the absolute folder it
/// belongs to (a system's ROM folder), and returns it relative to `base`: UTF-8, '/' between the elements, no "."
/// element, the case as written. Accepted:
///  - relative paths with '/' or '\' separators, with "./" or without ("./a/b.ext", "a\b.ext"); "." elements and
///    repeated separators are dropped;
///  - absolute paths with a drive letter ("D:\Roms\gb\a.gb", "D:/Roms/gb/a.gb") or a share ("\\nas\roms\gb\a.gb")
///    inside `base`, compared element by element with the file-system casing (pathIsWithin); what lies below `base`
///    is kept as written.
/// Errors, naming the text, of two kinds that callers tell apart by their code:
///  - OutOfRange: the path lies outside `base` (an absolute path elsewhere, on another drive or share, or ".."
///    elements that lead out of it): an ES-DE entry "outside the system folder" (ADR-0008 §3);
///  - InvalidArgument: the text is malformed or a form that is not allowed: an empty path, a NUL or another character
///    below U+0020 (which NTFS and exFAT refuse in names; DEL, U+007F, is allowed like any other name character),
///    malformed UTF-8, ".." elements that stay inside `base` (refused all the same), ':' anywhere but after
///    a drive letter (alternate data streams), the \\?\ and \\.\ forms, drive-relative forms ("C:a.ext"; "\a.ext",
///    rooted without a drive), a UNC path without a share, a path that names `base` itself; also a `base` that is not
///    absolute.
[[nodiscard]] Expected<std::string> resolveRelativePath(std::string_view pathText, const std::filesystem::path& base);

} // namespace rv::core
