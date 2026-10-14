#pragma once

#include <string_view>

namespace rv::core {

/// Records the calling thread as the main (render) thread. Call once at startup, before spawning workers.
void markMainThread() noexcept;

/// True when called from the thread recorded by markMainThread() (or when none was recorded).
[[nodiscard]] bool isMainThread() noexcept;

/// Logs an error when called off the main thread. Use at the top of functions that touch the OpenGL
/// context or other main-thread-only state (specification §15.3).
void assertMainThread(std::string_view what) noexcept;

} // namespace rv::core
