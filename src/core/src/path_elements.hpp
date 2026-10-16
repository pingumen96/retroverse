#pragma once

// Private to rv_core: paths split into elements that compare as the file system compares names.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rv::core::detail {

/// The elements of `path` made absolute and normalized as Windows does (extendedLengthPath, the \\?\ prefix dropped
/// again), each upper-cased with the file-system casing; a UNC path starts with the element "\\" so that a server
/// never compares equal to a drive. Nothing when the casing fails.
[[nodiscard]] std::optional<std::vector<std::wstring>> foldedPathElements(const std::filesystem::path& path);

} // namespace rv::core::detail
