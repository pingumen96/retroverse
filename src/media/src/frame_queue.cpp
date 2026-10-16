#include <rv/media/frame_queue.hpp>

#include <algorithm>
#include <utility>

namespace rv::media {

FrameQueue::FrameQueue(std::size_t capacity) : m_ring(std::max<std::size_t>(capacity, 1)) {}

PushResult FrameQueue::push(PooledFrame frame, std::stop_token stop) {
    // A frame not queued goes back to its pool when `frame` is destroyed on return, after the lock is released.
    if (!frame) {
        return PushResult::Refused;
    }
    const std::uint32_t generation = frame->timing.generation;
    {
        std::unique_lock lock(m_mutex);
        const bool ready = m_notFull.wait(
            lock, stop, [&] { return m_closed || generation < m_generation || m_count < m_ring.size(); });
        if (!ready || m_closed) {
            return PushResult::Refused;
        }
        if (generation < m_generation) {
            return PushResult::Discarded;
        }
        m_ring[(m_head + m_count) % m_ring.size()] = std::move(frame);
        ++m_count;
    }
    return PushResult::Queued;
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

void FrameQueue::discardOlderThan(std::uint32_t generation) {
    {
        const std::lock_guard lock(m_mutex);
        if (generation <= m_generation) {
            return;
        }
        m_generation = generation;
    }
    // The producer's generations only grow, so the frames to discard are at the front. One at a time, released outside
    // the lock like clear().
    for (;;) {
        PooledFrame stale;
        {
            const std::lock_guard lock(m_mutex);
            if (m_count == 0 || m_ring[m_head]->timing.generation >= generation) {
                break;
            }
            stale = std::move(m_ring[m_head]);
            m_head = (m_head + 1) % m_ring.size();
            --m_count;
        }
    }
    // Also wakes a producer waiting for room with a frame of an older generation: it discards it.
    m_notFull.notify_all();
}

std::uint32_t FrameQueue::generation() const {
    const std::lock_guard lock(m_mutex);
    return m_generation;
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
