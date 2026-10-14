#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::core {

inline constexpr std::uintmax_t kDefaultTextLimitBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uintmax_t kDefaultBinaryLimitBytes = 1024ull * 1024ull * 1024ull;

/// Reads a whole file. Files larger than `maxBytes` are rejected (untrusted inputs, spec §19.1).
[[nodiscard]] Expected<std::string> readTextFile(const std::filesystem::path& path,
                                                 std::uintmax_t maxBytes = kDefaultTextLimitBytes);
[[nodiscard]] Expected<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path,
                                                              std::uintmax_t maxBytes = kDefaultBinaryLimitBytes);

/// Writes `data` to a temporary file in the destination directory and renames it over `path`, so readers
/// never observe a partially written file. Parent directories are created when missing.
[[nodiscard]] Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data);
[[nodiscard]] Status writeTextFileAtomic(const std::filesystem::path& path, std::string_view text);

/// Last modification time, or nothing when the file cannot be queried.
[[nodiscard]] std::optional<std::filesystem::file_time_type> lastWriteTime(const std::filesystem::path& path) noexcept;

} // namespace rv::core
