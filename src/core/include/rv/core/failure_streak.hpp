#pragma once

#include <cstddef>
#include <optional>

#include <rv/core/expected.hpp>

namespace rv::core {

/// Decides which failures of an operation retried every frame are worth a log line, so that a persistent failure (a
/// view that cannot be drawn, say) is logged once instead of at the frame rate: the first failure of a streak, and each
/// failure whose error code differs from the last one logged. Successes end the streak and report how many failures it
/// counted, so the caller can log the recovery. Not thread-safe.
class FailureStreak {
public:
    /// Records a failed attempt with `code`. True when the caller should log it.
    [[nodiscard]] bool fail(ErrorCode code) noexcept {
        ++m_failures;
        if (m_loggedCode == code) {
            return false;
        }
        m_loggedCode = code;
        return true;
    }

    /// Records a successful attempt. The number of failed attempts of the streak it ends, or nothing when the previous
    /// attempt succeeded too.
    [[nodiscard]] std::optional<std::size_t> succeed() noexcept {
        if (m_failures == 0) {
            return std::nullopt;
        }
        const std::size_t failures = m_failures;
        m_failures = 0;
        m_loggedCode.reset();
        return failures;
    }

    /// Failed attempts since the last success.
    [[nodiscard]] std::size_t failures() const noexcept { return m_failures; }

private:
    std::optional<ErrorCode> m_loggedCode;
    std::size_t m_failures = 0;
};

} // namespace rv::core
