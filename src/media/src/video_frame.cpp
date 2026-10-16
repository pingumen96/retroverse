#include <rv/media/video_frame.hpp>

#include <algorithm>
#include <new>
#include <utility>

namespace rv::media {
namespace {

/// Alignment of frame pixels: a cache line, which also satisfies the SIMD paths of swscale and memcpy.
constexpr std::align_val_t kPixelAlignment{64};

} // namespace

// --- VideoFrame
// -------------------------------------------------------------------------------------------------------

void VideoFrame::AlignedDelete::operator()(std::uint8_t* pixels) const noexcept {
    ::operator delete[](pixels, kPixelAlignment);
}

bool VideoFrame::resize(FrameSize size) {
    if (!size.valid()) {
        return false;
    }
    const std::size_t needed = static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height) *
                               static_cast<std::size_t>(kFrameBytesPerPixel);
    bool allocated = false;
    if (needed > m_capacity) {
        m_pixels.reset(static_cast<std::uint8_t*>(::operator new[](needed, kPixelAlignment)));
        m_capacity = needed;
        allocated = true;
    }
    m_size = size;
    return allocated;
}

std::size_t VideoFrame::strideBytes() const noexcept {
    return static_cast<std::size_t>(m_size.width) * static_cast<std::size_t>(kFrameBytesPerPixel);
}

std::size_t VideoFrame::sizeBytes() const noexcept {
    return strideBytes() * static_cast<std::size_t>(m_size.height);
}

std::span<const std::uint8_t> VideoFrame::bytes() const noexcept {
    return {m_pixels.get(), m_pixels ? sizeBytes() : std::size_t{0}};
}

// --- PooledFrame
// ------------------------------------------------------------------------------------------------------

PooledFrame::PooledFrame(std::shared_ptr<FramePool> pool, VideoFrame* frame) noexcept
    : m_pool(std::move(pool)), m_frame(frame) {}

PooledFrame::PooledFrame(PooledFrame&& other) noexcept
    : m_pool(std::move(other.m_pool)), m_frame(std::exchange(other.m_frame, nullptr)) {}

PooledFrame& PooledFrame::operator=(PooledFrame&& other) noexcept {
    if (this != &other) {
        reset();
        m_pool = std::move(other.m_pool);
        m_frame = std::exchange(other.m_frame, nullptr);
    }
    return *this;
}

PooledFrame::~PooledFrame() {
    reset();
}

void PooledFrame::reset() noexcept {
    if (m_frame != nullptr && m_pool) {
        m_pool->release(m_frame);
    }
    m_frame = nullptr;
    m_pool.reset();
}

// --- FramePool
// --------------------------------------------------------------------------------------------------------

std::shared_ptr<FramePool> FramePool::create(std::size_t capacity) {
    return std::make_shared<FramePool>(Passkey{}, capacity);
}

FramePool::FramePool(Passkey, std::size_t capacity) {
    const std::size_t count = std::max<std::size_t>(capacity, 1);
    m_frames.reserve(count);
    m_free.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        m_frames.push_back(std::make_unique<VideoFrame>());
        m_free.push_back(m_frames.back().get());
    }
}

PooledFrame FramePool::acquire(std::stop_token stop) {
    std::unique_lock lock(m_mutex);
    if (!m_freed.wait(lock, stop, [this] { return m_closed || !m_free.empty(); }) || m_closed) {
        return {};
    }
    VideoFrame* frame = m_free.back();
    m_free.pop_back();
    lock.unlock();
    return PooledFrame(shared_from_this(), frame);
}

PooledFrame FramePool::tryAcquire() {
    std::unique_lock lock(m_mutex);
    if (m_closed || m_free.empty()) {
        return {};
    }
    VideoFrame* frame = m_free.back();
    m_free.pop_back();
    lock.unlock();
    return PooledFrame(shared_from_this(), frame);
}

void FramePool::close() {
    {
        const std::lock_guard lock(m_mutex);
        m_closed = true;
    }
    m_freed.notify_all();
}

std::size_t FramePool::available() const {
    const std::lock_guard lock(m_mutex);
    return m_free.size();
}

void FramePool::release(VideoFrame* frame) noexcept {
    {
        const std::lock_guard lock(m_mutex);
        // m_free's capacity is the pool's: never allocates.
        m_free.push_back(frame);
    }
    m_freed.notify_one();
}

} // namespace rv::media
