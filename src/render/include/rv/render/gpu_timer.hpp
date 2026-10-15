#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include <rv/core/expected.hpp>
#include <rv/render/gl_object.hpp>

namespace rv::gl {

/// Measures how long the GPU takes to execute a span of commands with GL_TIME_ELAPSED queries, without stalling: the
/// results are collected frames later from a ring of kQueryCount queries, and a span that finds every query still
/// waiting for its result is simply not measured. Spans must not nest (only one GL_TIME_ELAPSED query can be active).
/// Move-only; a default-constructed timer is empty and measures nothing. Main thread only.
class GpuTimer {
public:
    static constexpr std::size_t kQueryCount = 4;

    GpuTimer() noexcept = default;

    /// Creates the queries, labeled `debugName`. Errors: GraphicsError when the driver creates none.
    [[nodiscard]] static Expected<GpuTimer> create(std::string_view debugName);

    /// Starts a span (first collecting the results that arrived).
    void begin();
    /// Ends the span started by begin().
    void end();

    /// Duration of the most recent span whose result has arrived, in milliseconds; nothing before the first one.
    /// Collects the results that arrived, never waits for one.
    [[nodiscard]] std::optional<double> latestMilliseconds();

private:
    /// Reads the results of the pending queries in issue order, up to the first that is not available yet.
    void collect();

    std::array<QueryObject, kQueryCount> m_queries;
    std::array<bool, kQueryCount> m_pending{};
    /// Next query to use; also the oldest pending one when the ring is full.
    std::size_t m_next = 0;
    std::optional<std::size_t> m_active;
    std::optional<double> m_latest;
};

} // namespace rv::gl
