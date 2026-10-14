#include <rv/core/main_thread.hpp>

#include <atomic>
#include <thread>

#include <spdlog/spdlog.h>

namespace rv::core {
namespace {

std::atomic<std::thread::id> g_mainThread{};

} // namespace

void markMainThread() noexcept {
    g_mainThread.store(std::this_thread::get_id());
}

bool isMainThread() noexcept {
    const std::thread::id recorded = g_mainThread.load();
    return recorded == std::thread::id{} || recorded == std::this_thread::get_id();
}

void assertMainThread(std::string_view what) noexcept {
    if (!isMainThread()) {
        try {
            spdlog::error("Main-thread-only operation called from a worker thread: {}", what);
        } catch (...) {
            // Logging must never turn a diagnostic into a crash.
        }
    }
}

} // namespace rv::core
