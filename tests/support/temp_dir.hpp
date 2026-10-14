#pragma once

#include <filesystem>
#include <string_view>

namespace rv::test {

/// Unique temporary directory, removed recursively on destruction.
class TempDir {
public:
    explicit TempDir(std::string_view prefix = "rv-test");
    ~TempDir();

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    /// path() / relative
    [[nodiscard]] std::filesystem::path file(std::string_view relative) const;

private:
    std::filesystem::path m_path;
};

} // namespace rv::test
