#pragma once

// Internal validation of relative paths read from untrusted files (specification §19.1: directory traversal).
// Not part of the public API.

#include <string>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::assets::detail {

/// Validates a relative UTF-8 path and returns it in canonical form (segments joined by '/'). A valid result can be
/// passed to rv::core::pathFromUtf8, which throws on malformed UTF-8.
///
/// Rejected (InvalidArgument): empty paths, text that is not well-formed UTF-8, backslashes, absolute paths
/// ("/x"), drive letters and URI schemes (any ':'), control characters, empty segments ("a//b") and ".."
/// segments. "." segments are dropped when `allowDotSegments` is true (glTF URIs such as "./textures/a.png") and
/// rejected otherwise. Messages show unprintable bytes as \xHH.
[[nodiscard]] Expected<std::string> normalizeRelativePath(std::string_view path, bool allowDotSegments,
                                                          std::string_view context);

} // namespace rv::assets::detail
