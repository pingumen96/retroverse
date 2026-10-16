#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

#include <rv/media/video_frame.hpp>

namespace rv::media {

/// Bounded FIFO of decoded frames between one producer (a decoder thread) and one consumer (the render thread),
/// specification §9.4: the producer waits while it is full, so a decoder never runs more than its capacity ahead of
/// what is shown and its memory stays bounded. The consumer never waits for the producer: peek() and pop() hold the
/// lock only to read or move a handle. A fixed ring: pushing and popping allocate nothing. Thread-safe.
class FrameQueue {
public:
    /// Frames decoded ahead by default (spec §9.4: a queue of a few frames).
    static constexpr std::size_t kDefaultCapacity = 4;

    /// A queue of `capacity` frames (at least 1).
    explicit FrameQueue(std::size_t capacity = kDefaultCapacity);
    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;
    ~FrameQueue() = default;

    /// Appends `frame` (non-empty) at the back, waiting while the queue is full. Returns false, and lets the frame go
    /// back to its pool, when `stop` is requested or the queue is closed before there is room.
    [[nodiscard]] bool push(PooledFrame frame, std::stop_token stop);

    /// Timing of the frame `index` places from the front (0: the oldest); nothing past the back.
    [[nodiscard]] std::optional<FrameTiming> peek(std::size_t index = 0) const;
    /// Removes and returns the oldest frame; empty when the queue is empty.
    [[nodiscard]] PooledFrame pop();

    /// Removes every queued frame (they go back to their pool) and wakes a producer waiting for room.
    void clear();
    /// Wakes a waiting producer; push() fails from now on. Queued frames can still be popped.
    void close();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_ring.size(); }
    [[nodiscard]] bool closed() const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable_any m_notFull;
    std::vector<PooledFrame> m_ring;
    std::size_t m_head = 0;
    std::size_t m_count = 0;
    bool m_closed = false;
};

} // namespace rv::media
