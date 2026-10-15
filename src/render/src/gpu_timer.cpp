#include <rv/render/gpu_timer.hpp>

#include <cstdint>
#include <string>

#include <fmt/format.h>

#include "gl_object_label.hpp"

namespace rv::gl {

Expected<GpuTimer> GpuTimer::create(std::string_view debugName) {
    GpuTimer timer;
    for (std::size_t index = 0; index < kQueryCount; ++index) {
        GLuint id = 0;
        glCreateQueries(GL_TIME_ELAPSED, 1, &id);
        if (id == 0) {
            return makeError(ErrorCode::GraphicsError, "glCreateQueries returned no timer query",
                             std::string(debugName));
        }
        timer.m_queries[index] = QueryObject(id);
        detail::setObjectLabel(GL_QUERY, id, fmt::format("{}[{}]", debugName, index));
    }
    return timer;
}

void GpuTimer::collect() {
    // Queries complete in issue order: the oldest pending one is the first after the most recent.
    for (std::size_t step = 0; step < kQueryCount; ++step) {
        const std::size_t index = (m_next + step) % kQueryCount;
        if (!m_pending[index]) {
            continue;
        }
        GLint available = GL_FALSE;
        glGetQueryObjectiv(m_queries[index].id(), GL_QUERY_RESULT_AVAILABLE, &available);
        if (available == GL_FALSE) {
            return;
        }
        GLuint64 nanoseconds = 0;
        glGetQueryObjectui64v(m_queries[index].id(), GL_QUERY_RESULT, &nanoseconds);
        m_latest = static_cast<double>(nanoseconds) / 1.0e6;
        m_pending[index] = false;
    }
}

void GpuTimer::begin() {
    if (!m_queries[m_next].valid() || m_active) {
        return;
    }
    collect();
    if (m_pending[m_next]) {
        return; // every query still waits for its result: this span is not measured
    }
    glBeginQuery(GL_TIME_ELAPSED, m_queries[m_next].id());
    m_active = m_next;
}

void GpuTimer::end() {
    if (!m_active) {
        return;
    }
    glEndQuery(GL_TIME_ELAPSED);
    m_pending[*m_active] = true;
    m_next = (*m_active + 1) % kQueryCount;
    m_active.reset();
}

std::optional<double> GpuTimer::latestMilliseconds() {
    collect();
    return m_latest;
}

} // namespace rv::gl
