#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <vector>

namespace rv::media {

/// Bytes per pixel of the frames a VideoDecoder produces: RGBA8.
inline constexpr int kFrameBytesPerPixel = 4;

/// Width and height of a picture in pixels.
struct FrameSize {
    int width = 0;
    int height = 0;

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
    bool operator==(const FrameSize&) const = default;
};

/// When a frame is to be shown, as the presentation clock reads it.
struct FrameTiming {
    /// Presentation timestamp, seconds: the decoder's best-effort timestamp times the stream time base, measured from
    /// the start of the stream and increased by the length of the passes before it when the clip loops, so that it
    /// keeps growing across loops within a generation.
    double ptsSeconds = 0.0;
    /// How long the frame is meant to stay on screen, seconds (> 0).
    double durationSeconds = 0.0;
    /// Seek generation: grows each time the decoder seeks, so that frames decoded before a seek are recognized and
    /// discarded and the clock starts again at the first frame after it. Monotonic across loops within a generation.
    std::uint32_t generation = 0;
};

/// A decoded picture: RGBA8 pixels (sRGB-encoded, as the video stores them), rows top to bottom (ADR-0002), tightly
/// packed (strideBytes() = width * 4), in a 64-byte aligned allocation that is reused: resize() reallocates only when
/// the pixels outgrow it. Frames live in a FramePool and are handed around as PooledFrame.
class VideoFrame {
public:
    VideoFrame() noexcept = default;
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&&) = delete;
    VideoFrame& operator=(VideoFrame&&) = delete;
    ~VideoFrame() = default;

    /// Makes the frame `size` pixels (both sides at least 1; others leave it unchanged and return false). Returns true
    /// when the pixel storage had to be allocated, false when the current allocation was reused. Pixel contents are
    /// undefined afterwards.
    bool resize(FrameSize size);

    [[nodiscard]] FrameSize size() const noexcept { return m_size; }
    [[nodiscard]] int width() const noexcept { return m_size.width; }
    [[nodiscard]] int height() const noexcept { return m_size.height; }
    /// Bytes from the start of one row to the next: width * 4.
    [[nodiscard]] std::size_t strideBytes() const noexcept;
    /// width * height * 4.
    [[nodiscard]] std::size_t sizeBytes() const noexcept;
    /// Bytes allocated for pixels (at least sizeBytes()), not counting the padding past them that the converter may
    /// write into (I5 step 7).
    [[nodiscard]] std::size_t capacityBytes() const noexcept { return m_capacity; }

    [[nodiscard]] std::uint8_t* pixels() noexcept { return m_pixels.get(); }
    [[nodiscard]] const std::uint8_t* pixels() const noexcept { return m_pixels.get(); }
    /// The sizeBytes() bytes of the picture.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

    /// When to show the frame.
    FrameTiming timing;
    /// Position of the frame in decoding order across loops and seeks, from 0.
    std::uint64_t serial = 0;
    /// Pass of the clip the frame comes from: 0 for the first, 1 after the first loop, ...
    std::uint32_t loop = 0;

private:
    struct AlignedDelete {
        void operator()(std::uint8_t* pixels) const noexcept;
    };

    std::unique_ptr<std::uint8_t[], AlignedDelete> m_pixels;
    std::size_t m_capacity = 0;
    FrameSize m_size;
};

class FramePool;

/// Exclusive use of one frame of a FramePool, which gets it back when the handle is destroyed or reset. Move-only;
/// empty by default. The handle keeps the pool alive, so frames may outlive the decoder that filled them. Copying the
/// handle's pool reference touches an atomic counter only: no heap allocation.
class PooledFrame {
public:
    PooledFrame() noexcept = default;
    PooledFrame(PooledFrame&& other) noexcept;
    PooledFrame& operator=(PooledFrame&& other) noexcept;
    PooledFrame(const PooledFrame&) = delete;
    PooledFrame& operator=(const PooledFrame&) = delete;
    ~PooledFrame();

    [[nodiscard]] explicit operator bool() const noexcept { return m_frame != nullptr; }
    [[nodiscard]] VideoFrame* get() const noexcept { return m_frame; }
    [[nodiscard]] VideoFrame& operator*() const noexcept { return *m_frame; }
    [[nodiscard]] VideoFrame* operator->() const noexcept { return m_frame; }

    /// Returns the frame to its pool now; the handle becomes empty.
    void reset() noexcept;

private:
    friend class FramePool;
    PooledFrame(std::shared_ptr<FramePool> pool, VideoFrame* frame) noexcept;

    std::shared_ptr<FramePool> m_pool;
    VideoFrame* m_frame = nullptr;
};

/// A fixed set of reusable frames shared by a decoder thread, which fills them, and the render thread, which shows them
/// and gives them back: decoding allocates no frame in steady state, and the memory a decoder holds is bounded by the
/// pool's capacity. Thread-safe. Create it with create(); handles keep it alive.
class FramePool : public std::enable_shared_from_this<FramePool> {
public:
    /// A pool of `capacity` frames (at least 1). Pixel storage is allocated by VideoFrame::resize() on first use.
    [[nodiscard]] static std::shared_ptr<FramePool> create(std::size_t capacity);

    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;
    ~FramePool() = default;

    /// Takes a free frame, waiting while every frame is in use. Empty when `stop` is requested or the pool is closed
    /// while waiting.
    [[nodiscard]] PooledFrame acquire(std::stop_token stop);
    /// Takes a free frame without waiting; empty when none is free or the pool is closed.
    [[nodiscard]] PooledFrame tryAcquire();

    /// Wakes every waiting acquire(), which then returns empty, as do later calls. Frames in use still come back.
    void close();

    [[nodiscard]] std::size_t capacity() const noexcept { return m_frames.size(); }
    /// Frames not in use right now.
    [[nodiscard]] std::size_t available() const;

private:
    struct Passkey {};

public:
    /// Use create(); public only for std::make_shared.
    FramePool(Passkey, std::size_t capacity);

private:
    friend class PooledFrame;
    void release(VideoFrame* frame) noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable_any m_freed;
    std::vector<std::unique_ptr<VideoFrame>> m_frames;
    /// Free frames; its capacity is the pool's, so returning a frame never allocates.
    std::vector<VideoFrame*> m_free;
    bool m_closed = false;
};

} // namespace rv::media
