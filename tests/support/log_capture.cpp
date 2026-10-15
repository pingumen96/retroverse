#include "support/log_capture.hpp"

#include <spdlog/sinks/ostream_sink.h>

namespace rv::test {

LogCapture::LogCapture() : m_previous(spdlog::default_logger()) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(m_stream);
    sink->set_pattern("%l %v");
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("rv-test-capture", sink));
}

LogCapture::~LogCapture() {
    spdlog::set_default_logger(m_previous);
}

std::size_t LogCapture::count(std::string_view needle) const {
    spdlog::default_logger()->flush();
    const std::string captured = m_stream.str();
    std::size_t found = 0;
    for (std::size_t pos = captured.find(needle); pos != std::string::npos; pos = captured.find(needle, pos + 1)) {
        ++found;
    }
    return found;
}

std::string LogCapture::text() const {
    spdlog::default_logger()->flush();
    return m_stream.str();
}

} // namespace rv::test
