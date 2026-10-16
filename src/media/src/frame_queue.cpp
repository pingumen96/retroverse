#include <rv/media/frame_queue.hpp>

#include <algorithm>
#include <utility>

namespace rv::media {

FrameQueue::FrameQueue(std::size_t capacity) : m_ring(std::max<std::size_t>(capacity, 1)) {}

bool FrameQueue::push(PooledFrame frame, std::stop_token stop) {
    if (!frame) {
        return false;
    }
    {
        std::unique_lock lock(m_mutex);
        if (!m_notFull.wait(lock, stop, [this] { return m_closed || m_count < m_ring.size(); }) || m_closed) {
            return false; // `frame` goes back to its pool on return, outside the lock
        }
        m_ring[(m_head + m_count) % m_ring.size()] = std::move(frame);
        ++m_count;
    }
    return true;
}

std::optional<FrameTiming> FrameQueue::peek(std::size_t index) const {
    const std::lock_guard lock(m_mutex);
    if (index >= m_count) {
        return std::nullopt;
    }
    return m_ring[(m_head + index) % m_ring.size()]->timing;
}

PooledFrame FrameQueue::pop() {
    PooledFrame frame;
    {
        const std::lock_guard lock(m_mutex);
        if (m_count == 0) {
            return frame;
        }
        frame = std::move(m_ring[m_head]);
        m_head = (m_head + 1) % m_ring.size();
        --m_count;
    }
    m_notFull.notify_one();
    return frame;
}

void FrameQueue::clear() {
    // One frame at a time, released outside the lock (a frame going back to its pool takes the pool's lock).
    for (;;) {
        PooledFrame frame = pop();
        if (!frame) {
            break;
        }
    }
    m_notFull.notify_all();
}

void FrameQueue::close() {
    {
        const std::lock_guard lock(m_mutex);
        m_closed = true;
    }
    m_notFull.notify_all();
}

std::size_t FrameQueue::size() const {
    const std::lock_guard lock(m_mutex);
    return m_count;
}

bool FrameQueue::closed() const {
    const std::lock_guard lock(m_mutex);
    return m_closed;
}

} // namespace rv::media
