#include <rv/app/controller_watch.hpp>

namespace rv::app {

void ControllerWatch::update(bool connected, double deltaSeconds) noexcept {
    if (m_connected && *m_connected && !connected) {
        ++m_lost;
        m_left = kNoticeSeconds;
    } else if (m_connected && !*m_connected && connected) {
        ++m_back;
        m_left = 0.0;
    } else if (m_left > 0.0) {
        m_left -= deltaSeconds;
    }
    m_connected = connected;
}

} // namespace rv::app
