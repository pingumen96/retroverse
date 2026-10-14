#pragma once

// Internal validation of relative paths read from untrusted files (specification §19.1: directory traversal).
// Not part of the public API.

#include <string>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::assets::detail {

/// Validates a relative UTF-8 path and returns it in canonical form (segments joined by '/').
///
/// Rejected: empty paths, backslashes, absolute paths ("/x"), drive letters and URI schemes (any ':'),
/// control characters, empty segments ("a//b") and ".." segments. "." segments are dropped when
/// `allowDotSegments` is true (glTF URIs such as "./textures/a.png") and rejected otherwise.
[[nodiscard]] Expected<std::string> normalizeRelativePath(std::string_view path, bool allowDotSegments,
                                                          std::string_view context);

} // namespace rv::assets::detail
