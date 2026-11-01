#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// FR-029, ADR-0018 §7: the last pad going and one coming back, and how long "Controller scollegato" stays. Fed once per
// frame, so a pad lost during a game (no frames run) shows when the frames resume, if none came back meanwhile.

#include <cstdint>
#include <optional>

namespace rv::app {

class ControllerWatch {
public:
    /// How long the notice stays when no pad comes back.
    static constexpr double kNoticeSeconds = 8.0;

    /// A pad is plugged in on this frame, `deltaSeconds` after the last.
    void update(bool connected, double deltaSeconds) noexcept;

    [[nodiscard]] bool noticeShown() const noexcept { return m_left > 0.0; }
    /// Times the last pad went, and times one came back after that.
    [[nodiscard]] std::uint64_t lost() const noexcept { return m_lost; }
    [[nodiscard]] std::uint64_t back() const noexcept { return m_back; }

private:
    std::optional<bool> m_connected;
    double m_left = 0.0;
    std::uint64_t m_lost = 0;
    std::uint64_t m_back = 0;
};

} // namespace rv::app
