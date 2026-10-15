#pragma once

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace rv::test {

/// Captures what spdlog's default logger prints while alive: each line reads "<level> <message>" ("warning ...",
/// "error ..."). The previous default logger is restored on destruction.
class LogCapture {
public:
    LogCapture();
    ~LogCapture();
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    /// Occurrences of `needle` in the captured text.
    [[nodiscard]] std::size_t count(std::string_view needle) const;
    /// Everything captured so far.
    [[nodiscard]] std::string text() const;

private:
    std::ostringstream m_stream;
    std::shared_ptr<spdlog::logger> m_previous;
};

} // namespace rv::test
