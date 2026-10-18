#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <glad/gl.h>
#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/buffer.hpp>
#include <rv/render/gl_object.hpp>
#include <rv/render/gpu_memory.hpp>

namespace rv::gl {

/// Outcome of VideoTexture::upload().
enum class VideoUpload : std::uint8_t {
    /// The frame was copied into a pixel buffer and the texture update from it was queued.
    Uploaded,
    /// The next pixel buffer is still being read by the GPU: nothing was done (no wait). Keep the frame and try again
    /// at the next render frame.
    RingBusy,
};

/// How a VideoTexture learns that the GPU is done with a pixel buffer (ADR-0004).
enum class VideoUploadSync : std::uint8_t {
    /// The GL_TIMESTAMP query written after the copy: OpenGL 4.5 §4.3 records it once every earlier command has fully
    /// completed, so its result being available means the buffer was read. Costs nothing more than the GPU timing,
    /// whereas a fence created after the copy makes NVIDIA's threaded driver wait for its server thread (about 0.5 ms
    /// per 720p upload on the reference machine). The default where the context has timestamps
    /// (GL_QUERY_COUNTER_BITS > 0).
    TimestampQuery,
    /// A fence (glFenceSync) after the copy, tested with glClientWaitSync and a zero timeout. The fallback without
    /// timestamps.
    Fence,
};

/// "timestamp query" or "fence".
[[nodiscard]] std::string_view toString(VideoUploadSync sync) noexcept;

/// Counters of a VideoTexture.
struct VideoTextureStats {
    std::uint64_t uploads = 0;
    /// Uploads skipped because the next pixel buffer was still in use (VideoUpload::RingBusy).
    std::uint64_t ringBusy = 0;
    /// Texture reallocations after the video changed size (the first allocation not included).
    std::uint64_t reallocations = 0;
    /// GPU timings dropped because pollGpuMilliseconds() was not called often enough, or the ring was reallocated.
    std::uint64_t gpuTimingsLost = 0;
    /// CPU time of the last upload's copy into the pixel buffer alone (the rest of upload() is GL calls), milliseconds.
    double lastCopyMilliseconds = 0.0;
};

/// The picture of a playing video on the GPU (specification §9.4: texture upload through pixel buffers on the render
/// thread): an immutable GL_SRGB8_ALPHA8 texture the size of the video (sampling returns linear values; bilinear,
/// clamped to its edges, one level), refilled from a ring of kRingSize pixel unpack buffers (PBOs) in one buffer,
/// persistently and coherently mapped (glNamedBufferStorage with GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT): an
/// upload copies the RGBA8 frame into the next buffer and queues glTextureSubImage2D from it between two GL_TIMESTAMP
/// queries; the GPU copies the pixels while the CPU goes on. A buffer is written again only once the GPU is done with
/// it (VideoUploadSync), tested without waiting, so an upload never stalls the render thread: when the next buffer is
/// still busy the upload is skipped (RingBusy). The timestamps also time each upload on the GPU, read back frames later
/// (pollGpuMilliseconds()). Frames of another size reallocate the texture (and the ring when it is too small). GL
/// objects carry debug labels. Move-only; a default-constructed or moved-from texture is empty. Main thread only, like
/// every GL call (decoder threads hand their frames over: rv::media::VideoPlayback).
class VideoTexture {
public:
    /// Pixel buffers used in turn: the frame being written, and up to two the GPU may still be copying from.
    static constexpr std::size_t kRingSize = 3;
    /// Completed GPU timings kept until polled.
    static constexpr std::size_t kMaxGpuTimings = 16;

    VideoTexture() noexcept = default;
    VideoTexture(VideoTexture&& other) noexcept;
    VideoTexture& operator=(VideoTexture&& other) noexcept;
    VideoTexture(const VideoTexture&) = delete;
    VideoTexture& operator=(const VideoTexture&) = delete;
    ~VideoTexture() = default;

    /// Creates the texture (cleared to black) and the pixel buffer ring for frames of `size` pixels, labeled
    /// `debugName`, synchronized with `sync` (Fence when the context has no timestamps). Errors: InvalidArgument for an
    /// empty size, OutOfRange beyond GL_MAX_TEXTURE_SIZE, GraphicsError when an allocation, the mapping or a query
    /// fails.
    [[nodiscard]] static Expected<VideoTexture> create(glm::ivec2 size, std::string_view debugName,
                                                       VideoUploadSync sync = VideoUploadSync::TimestampQuery);

    /// Uploads an RGBA8 frame of `size` pixels whose rows, top row first (ADR-0002: texture coordinate t = 0 samples
    /// it), start `strideBytes` apart in `rgba`. Never waits for the GPU (see VideoUpload). A size other than size()
    /// reallocates first. Errors: InvalidArgument for an empty texture, an empty size, a stride below width * 4 or too
    /// few bytes; the errors of create() when a reallocation fails (the texture then keeps its previous size).
    [[nodiscard]] Expected<VideoUpload> upload(std::span<const std::uint8_t> rgba, glm::ivec2 size,
                                               std::size_t strideBytes);

    /// GPU duration of the oldest upload whose timing has arrived and was not polled yet, milliseconds; nothing when
    /// none has. Never waits.
    [[nodiscard]] std::optional<double> pollGpuMilliseconds();

    /// Binds the texture to texture unit `unit` (glBindTextureUnit).
    void bind(GLuint unit) const;

    [[nodiscard]] GLuint textureId() const noexcept { return m_texture.id(); }
    [[nodiscard]] bool valid() const noexcept { return m_texture.valid(); }
    [[nodiscard]] glm::ivec2 size() const noexcept { return m_size; }
    /// The synchronization in use (Fence when TimestampQuery was asked for without timestamps).
    [[nodiscard]] VideoUploadSync sync() const noexcept { return m_sync; }
    [[nodiscard]] const VideoTextureStats& stats() const noexcept { return m_stats; }
    /// GPU memory of the texture and the pixel buffer ring, bytes (recorded in gpuMemoryLedger() as
    /// GpuMemoryKind::Video).
    [[nodiscard]] std::size_t gpuBytes() const noexcept;
    [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }

private:
    /// A new texture of `size`; a new ring when its buffers are too small for it.
    [[nodiscard]] Status allocate(glm::ivec2 size);
    /// True once the GPU is done with the pixel buffer of `slot` (collecting its timing when it arrived).
    [[nodiscard]] bool slotFree(std::size_t slot);
    /// Moves the GPU timings that arrived into m_timings, oldest first.
    void collectGpuTimings();
    /// Reads the timing of `slot` if it arrived; true when the slot has no pending timing left.
    bool collectSlotTiming(std::size_t slot);
    void pushTiming(double milliseconds) noexcept;

    TextureObject m_texture;
    /// m_texture's storage, recorded as GpuMemoryKind::Video (the ring records itself through its Buffer).
    GpuMemoryAllocation m_textureMemory;
    glm::ivec2 m_size{0};
    VideoUploadSync m_sync = VideoUploadSync::TimestampQuery;
    Buffer m_ring;
    /// Persistent, coherent mapping of m_ring; valid while m_ring lives.
    std::byte* m_mapped = nullptr;
    std::size_t m_slotBytes = 0;
    /// VideoUploadSync::Fence: one per pixel buffer.
    std::array<FenceSync, kRingSize> m_fences;
    /// GL_TIMESTAMP queries around the copy of each slot: begin, end.
    std::array<std::array<QueryObject, 2>, kRingSize> m_queries;
    std::array<bool, kRingSize> m_timingPending{};
    /// Slot of the next upload; the oldest upload in flight is at the same index.
    std::size_t m_nextSlot = 0;
    std::array<double, kMaxGpuTimings> m_timings{};
    std::size_t m_timingsHead = 0;
    std::size_t m_timingsCount = 0;
    VideoTextureStats m_stats;
    std::string m_debugName;
};

} // namespace rv::gl
