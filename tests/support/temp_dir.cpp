#include "support/temp_dir.hpp"

#include <atomic>
#include <chrono>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#include <rv/core/text.hpp>

namespace rv::test {
namespace {

std::atomic<unsigned> g_counter{0};

} // namespace

TempDir::TempDir(std::string_view prefix) {
    std::random_device device;
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::string name = std::string(prefix) + "-" + std::to_string(device()) + "-" +
                                 std::to_string(static_cast<unsigned long long>(ticks)) + "-" +
                                 std::to_string(g_counter.fetch_add(1));
        const std::filesystem::path candidate = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec) && !ec) {
            m_path = candidate;
            return;
        }
    }
    throw std::runtime_error("cannot create a unique temporary directory");
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
}

std::filesystem::path TempDir::file(std::string_view relative) const {
    return m_path / core::pathFromUtf8(relative);
}

} // namespace rv::test
