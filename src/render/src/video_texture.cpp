#include <rv/render/video_texture.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <utility>

#include <fmt/format.h>

#include "gl_object_label.hpp"

namespace rv::gl {
namespace {

constexpr GLbitfield kRingFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
/// Pixel buffer offsets are kept at a multiple of this (any multiple of 4 would do for RGBA8 rows).
constexpr std::size_t kSlotAlignment = 256;
constexpr std::size_t kBytesPerPixel = 4;

std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

/// True when the context records GL_TIMESTAMP queries (an implementation may report 0 counter bits).
bool hasTimestamps() noexcept {
    GLint bits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
    return bits > 0;
}

} // namespace

std::string_view toString(VideoUploadSync sync) noexcept {
    return sync == VideoUploadSync::Fence ? "fence" : "timestamp query";
}

VideoTexture::VideoTexture(VideoTexture&& other) noexcept
    : m_texture(std::move(other.m_texture)), m_size(std::exchange(other.m_size, glm::ivec2(0))), m_sync(other.m_sync),
      m_ring(std::move(other.m_ring)), m_mapped(std::exchange(other.m_mapped, nullptr)),
      m_slotBytes(std::exchange(other.m_slotBytes, std::size_t{0})), m_fences(std::move(other.m_fences)),
      m_queries(std::move(other.m_queries)), m_timingPending(std::exchange(other.m_timingPending, {})),
      m_nextSlot(std::exchange(other.m_nextSlot, std::size_t{0})), m_timings(other.m_timings),
      m_timingsHead(std::exchange(other.m_timingsHead, std::size_t{0})),
      m_timingsCount(std::exchange(other.m_timingsCount, std::size_t{0})), m_stats(std::exchange(other.m_stats, {})),
      m_debugName(std::move(other.m_debugName)) {}

VideoTexture& VideoTexture::operator=(VideoTexture&& other) noexcept {
    if (this != &other) {
        m_texture = std::move(other.m_texture);
        m_size = std::exchange(other.m_size, glm::ivec2(0));
        m_sync = other.m_sync;
        m_ring = std::move(other.m_ring);
        m_mapped = std::exchange(other.m_mapped, nullptr);
        m_slotBytes = std::exchange(other.m_slotBytes, std::size_t{0});
        m_fences = std::move(other.m_fences);
        m_queries = std::move(other.m_queries);
        m_timingPending = std::exchange(other.m_timingPending, {});
        m_nextSlot = std::exchange(other.m_nextSlot, std::size_t{0});
        m_timings = other.m_timings;
        m_timingsHead = std::exchange(other.m_timingsHead, std::size_t{0});
        m_timingsCount = std::exchange(other.m_timingsCount, std::size_t{0});
        m_stats = std::exchange(other.m_stats, {});
        m_debugName = std::move(other.m_debugName);
    }
    return *this;
}

Expected<VideoTexture> VideoTexture::create(glm::ivec2 size, std::string_view debugName, VideoUploadSync sync) {
    VideoTexture texture;
    texture.m_debugName = std::string(debugName);
    const bool timestamps = hasTimestamps();
    texture.m_sync = timestamps ? sync : VideoUploadSync::Fence;
    if (timestamps) {
        for (std::size_t slot = 0; slot < kRingSize; ++slot) {
            for (std::size_t index = 0; index < 2; ++index) {
                GLuint id = 0;
                glCreateQueries(GL_TIMESTAMP, 1, &id);
                if (id == 0) {
                    return makeError(ErrorCode::GraphicsError, "glCreateQueries returned no timestamp query",
                                     texture.m_debugName);
                }
                texture.m_queries[slot][index] = QueryObject(id);
                detail::setObjectLabel(GL_QUERY, id,
                                       fmt::format("{} upload {}[{}]", debugName, index == 0 ? "begin" : "end", slot));
            }
        }
    }
    if (auto allocated = texture.allocate(size); !allocated) {
        return propagate(allocated.error());
    }
    return texture;
}

Status VideoTexture::allocate(glm::ivec2 size) {
    if (size.x < 1 || size.y < 1) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("video size {}x{} is empty", size.x, size.y),
                         m_debugName);
    }
    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (size.x > maxSize || size.y > maxSize) {
        return makeError(ErrorCode::OutOfRange,
                         fmt::format("{}x{} exceeds the largest texture side, {}", size.x, size.y, maxSize),
                         m_debugName);
    }

    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &id);
    TextureObject texture(id);
    if (id == 0) {
        return makeError(ErrorCode::GraphicsError, "glCreateTextures returned no texture", m_debugName);
    }
    glTextureStorage2D(id, 1, GL_SRGB8_ALPHA8, size.x, size.y);
    GLint immutable = GL_FALSE;
    glGetTextureParameteriv(id, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
    if (immutable == GL_FALSE) {
        return makeError(ErrorCode::GraphicsError,
                         fmt::format("could not allocate a {}x{} video texture", size.x, size.y), m_debugName);
    }
    glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    constexpr std::array<std::uint8_t, 4> kBlack{0, 0, 0, 255};
    glClearTexImage(id, 0, GL_RGBA, GL_UNSIGNED_BYTE, kBlack.data());
    detail::setObjectLabel(GL_TEXTURE, id, m_debugName);

    const std::size_t slotBytes =
        alignUp(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * kBytesPerPixel, kSlotAlignment);
    if (!m_ring.valid() || slotBytes > m_slotBytes) {
        auto ring = Buffer::create(kRingSize * slotBytes, kRingFlags, m_debugName + " pixel buffers");
        if (!ring) {
            return propagate(ring.error());
        }
        void* mapped = glMapNamedBufferRange(ring->id(), 0, static_cast<GLsizeiptr>(ring->size()), kRingFlags);
        if (mapped == nullptr) {
            return makeError(ErrorCode::GraphicsError, "the pixel buffers cannot be mapped", m_debugName);
        }
        // The old ring (deleted, and so unmapped, by the assignment) lives on in the driver until the copies that read
        // it are done; its fences and timings no longer matter.
        for (std::size_t slot = 0; slot < kRingSize; ++slot) {
            m_fences[slot].reset();
            if (m_timingPending[slot]) {
                ++m_stats.gpuTimingsLost;
                m_timingPending[slot] = false;
            }
        }
        m_ring = std::move(*ring);
        m_mapped = static_cast<std::byte*>(mapped);
        m_slotBytes = slotBytes;
        m_nextSlot = 0;
    }
    m_texture = std::move(texture);
    m_size = size;
    return ok();
}

bool VideoTexture::slotFree(std::size_t slot) {
    if (m_sync == VideoUploadSync::TimestampQuery) {
        // The end timestamp of the slot's last copy has arrived only once that copy completed.
        return collectSlotTiming(slot);
    }
    if (!m_fences[slot].clientWait(0)) {
        return false;
    }
    m_fences[slot].reset();
    if (m_timingPending[slot] && !collectSlotTiming(slot)) {
        ++m_stats.gpuTimingsLost; // the fence passed before the timestamps arrived: do not wait for them
        m_timingPending[slot] = false;
    }
    return true;
}

Expected<VideoUpload> VideoTexture::upload(std::span<const std::uint8_t> rgba, glm::ivec2 size,
                                           std::size_t strideBytes) {
    if (!valid() || m_mapped == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "cannot upload to an empty video texture", m_debugName);
    }
    if (size.x < 1 || size.y < 1) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("frame size {}x{} is empty", size.x, size.y),
                         m_debugName);
    }
    const std::size_t rowBytes = static_cast<std::size_t>(size.x) * kBytesPerPixel;
    const auto rows = static_cast<std::size_t>(size.y);
    if (strideBytes < rowBytes || rgba.size() < strideBytes * (rows - 1) + rowBytes) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("{} bytes with a stride of {} do not hold a {}x{} RGBA8 frame", rgba.size(),
                                     strideBytes, size.x, size.y),
                         m_debugName);
    }
    if (size != m_size) {
        if (auto allocated = allocate(size); !allocated) {
            return propagate(allocated.error());
        }
        ++m_stats.reallocations;
    }

    collectGpuTimings();
    const std::size_t slot = m_nextSlot;
    if (!slotFree(slot)) {
        ++m_stats.ringBusy; // the GPU still copies from this buffer: never wait for it
        return VideoUpload::RingBusy;
    }

    const auto copyStart = std::chrono::steady_clock::now();
    std::byte* destination = m_mapped + slot * m_slotBytes;
    if (strideBytes == rowBytes) {
        std::memcpy(destination, rgba.data(), rowBytes * rows);
    } else {
        for (std::size_t row = 0; row < rows; ++row) {
            std::memcpy(destination + row * rowBytes, rgba.data() + row * strideBytes, rowBytes);
        }
    }
    m_stats.lastCopyMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copyStart).count();

    const bool timed = m_queries[slot][0].valid();
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_ring.id());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (timed) {
        glQueryCounter(m_queries[slot][0].id(), GL_TIMESTAMP);
    }
    // With a pixel unpack buffer bound, the last argument is an offset into it.
    glTextureSubImage2D(m_texture.id(), 0, 0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE,
                        reinterpret_cast<const void*>(slot * m_slotBytes));
    if (timed) {
        glQueryCounter(m_queries[slot][1].id(), GL_TIMESTAMP);
        m_timingPending[slot] = true;
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (m_sync == VideoUploadSync::Fence) {
        m_fences[slot] = FenceSync::insert();
    }
    m_nextSlot = (slot + 1) % kRingSize;
    ++m_stats.uploads;
    return VideoUpload::Uploaded;
}

bool VideoTexture::collectSlotTiming(std::size_t slot) {
    if (!m_timingPending[slot]) {
        return true;
    }
    GLint available = GL_FALSE;
    glGetQueryObjectiv(m_queries[slot][1].id(), GL_QUERY_RESULT_AVAILABLE, &available);
    if (available == GL_FALSE) {
        return false;
    }
    GLuint64 begin = 0;
    GLuint64 end = 0;
    glGetQueryObjectui64v(m_queries[slot][0].id(), GL_QUERY_RESULT, &begin);
    glGetQueryObjectui64v(m_queries[slot][1].id(), GL_QUERY_RESULT, &end);
    m_timingPending[slot] = false;
    pushTiming(end >= begin ? static_cast<double>(end - begin) / 1.0e6 : 0.0);
    return true;
}

void VideoTexture::collectGpuTimings() {
    // Uploads complete in submission order; the oldest one in flight uses the slot of the next upload.
    for (std::size_t step = 0; step < kRingSize; ++step) {
        if (!collectSlotTiming((m_nextSlot + step) % kRingSize)) {
            return;
        }
    }
}

void VideoTexture::pushTiming(double milliseconds) noexcept {
    if (m_timingsCount == kMaxGpuTimings) {
        m_timingsHead = (m_timingsHead + 1) % kMaxGpuTimings; // drop the oldest
        --m_timingsCount;
        ++m_stats.gpuTimingsLost;
    }
    m_timings[(m_timingsHead + m_timingsCount) % kMaxGpuTimings] = milliseconds;
    ++m_timingsCount;
}

std::optional<double> VideoTexture::pollGpuMilliseconds() {
    if (valid()) {
        collectGpuTimings();
    }
    if (m_timingsCount == 0) {
        return std::nullopt;
    }
    const double milliseconds = m_timings[m_timingsHead];
    m_timingsHead = (m_timingsHead + 1) % kMaxGpuTimings;
    --m_timingsCount;
    return milliseconds;
}

void VideoTexture::bind(GLuint unit) const {
    glBindTextureUnit(unit, m_texture.id());
}

std::size_t VideoTexture::gpuBytes() const noexcept {
    return static_cast<std::size_t>(m_size.x) * static_cast<std::size_t>(m_size.y) * kBytesPerPixel + m_ring.size();
}

} // namespace rv::gl
