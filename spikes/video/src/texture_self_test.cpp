#include "texture_self_test.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <glad/gl.h>
#include <glm/vec2.hpp>
#include <spdlog/spdlog.h>

#include <rv/render/gl_object.hpp>
#include <rv/render/video_texture.hpp>

namespace rv::spike {
namespace {

constexpr std::size_t kBytesPerPixel = 4;
/// Padding bytes after each row of a padded frame: never uploaded.
constexpr std::uint8_t kPadding = 0x5A;
/// Bursts tried before giving up on finding the ring busy (one is enough unless the thread is preempted mid-burst).
constexpr int kMaxBursts = 3;
/// GPU work queued before a burst: copies between two textures of this side, 16 MiB each, about 10 ms on the
/// reference GPU (more on slower ones); the burst's uploads wait behind it on the GPU.
constexpr int kBusySide = 2048;
constexpr int kBusyCopies = 160;

/// A texture of `side` x `side` RGBA8 texels for occupyGpu().
gl::TextureObject busyTexture(int side) {
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &id);
    gl::TextureObject texture(id);
    if (id != 0) {
        glTextureStorage2D(id, 1, GL_RGBA8, side, side);
    }
    return texture;
}

/// Queues `copies` texture copies back and forth between `first` and `second` and returns at once: GPU time, almost no
/// CPU time. Without it the GPU copies a small frame microseconds after the upload is issued, before the next upload
/// could find its pixel buffer still in use.
void occupyGpu(const gl::TextureObject& first, const gl::TextureObject& second, int side, int copies) {
    for (int copy = 0; copy < copies; ++copy) {
        const GLuint from = copy % 2 == 0 ? first.id() : second.id();
        const GLuint to = copy % 2 == 0 ? second.id() : first.id();
        glCopyImageSubData(from, GL_TEXTURE_2D, 0, 0, 0, 0, to, GL_TEXTURE_2D, 0, 0, 0, 0, side, side, 1);
    }
}

/// A frame of `size` pixels with rows `stride` bytes apart whose bytes are 0 or 255, different for each `seed`.
std::vector<std::uint8_t> patternFrame(glm::ivec2 size, std::size_t stride, int seed) {
    std::vector<std::uint8_t> bytes(stride * static_cast<std::size_t>(size.y), kPadding);
    for (int y = 0; y < size.y; ++y) {
        for (int x = 0; x < size.x; ++x) {
            std::uint8_t* pixel =
                bytes.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * kBytesPerPixel;
            for (int channel = 0; channel < 3; ++channel) {
                pixel[channel] = (x * 3 + y * 5 + channel * 7 + seed) % 4 < 2 ? std::uint8_t{255} : std::uint8_t{0};
            }
            pixel[3] = 255;
        }
    }
    return bytes;
}

/// The texture's texels as stored, texture row 0 (the frame's top row, ADR-0002) first.
std::vector<std::uint8_t> readBack(const gl::VideoTexture& texture) {
    const glm::ivec2 size = texture.size();
    std::vector<std::uint8_t> texels(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) *
                                     kBytesPerPixel);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTextureImage(texture.textureId(), 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(texels.size()),
                      texels.data());
    return texels;
}

/// True when the texture holds exactly the pixels of `frame` (rows `stride` bytes apart).
bool holds(const gl::VideoTexture& texture, const std::vector<std::uint8_t>& frame, std::size_t stride) {
    const glm::ivec2 size = texture.size();
    const std::vector<std::uint8_t> texels = readBack(texture);
    const std::size_t rowBytes = static_cast<std::size_t>(size.x) * kBytesPerPixel;
    for (int y = 0; y < size.y; ++y) {
        const auto row = static_cast<std::size_t>(y);
        if (!std::equal(texels.begin() + static_cast<std::ptrdiff_t>(row * rowBytes),
                        texels.begin() + static_cast<std::ptrdiff_t>((row + 1) * rowBytes),
                        frame.begin() + static_cast<std::ptrdiff_t>(row * stride))) {
            return false;
        }
    }
    return true;
}

std::size_t alignedSlotBytes(glm::ivec2 size) {
    constexpr std::size_t kSlotAlignment = 256; // gl::VideoTexture's slot alignment
    const std::size_t bytes = static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * kBytesPerPixel;
    return (bytes + kSlotAlignment - 1) / kSlotAlignment * kSlotAlignment;
}

} // namespace

nlohmann::json TextureSelfTestResult::toJson() const {
    return nlohmann::json{{"passed", passed},        {"failures", failures},
                          {"uploads", uploads},      {"reallocations", reallocations},
                          {"ringBusy", ringBusy},    {"burstsUntilBusy", burstsUntilBusy},
                          {"gpuTimings", gpuTimings}};
}

TextureSelfTestResult runTextureSelfTest() {
    TextureSelfTestResult result;
    const auto expect = [&result](bool condition, std::string what) {
        if (!condition) {
            result.failures.push_back(std::move(what));
        }
    };
    constexpr glm::ivec2 kFirst{64, 32};
    constexpr glm::ivec2 kLarger{96, 48};
    constexpr glm::ivec2 kSmaller{32, 16};
    auto created = gl::VideoTexture::create(kFirst, "spike self-test video");
    if (!created) {
        result.failures.push_back("create: " + created.error().toString());
        return result;
    }
    gl::VideoTexture& texture = *created;
    // Uploads `frame`, expecting `outcome`.
    const auto upload = [&](const std::vector<std::uint8_t>& frame, glm::ivec2 size, std::size_t stride,
                            gl::VideoUpload outcome, const std::string& what) {
        const auto uploaded = texture.upload(std::span<const std::uint8_t>(frame), size, stride);
        if (!uploaded) {
            result.failures.push_back(what + ": " + uploaded.error().toString());
            return false;
        }
        return *uploaded == outcome;
    };

    // 1. A frame of the texture's size, tightly packed.
    const std::size_t firstStride = static_cast<std::size_t>(kFirst.x) * kBytesPerPixel;
    const auto first = patternFrame(kFirst, firstStride, 1);
    expect(upload(first, kFirst, firstStride, gl::VideoUpload::Uploaded, "first frame"), "the first frame uploads");
    expect(holds(texture, first, firstStride), "the texture holds the first frame, top row first");

    // 2. A larger frame with padded rows: the texture is reallocated and the ring grows.
    const std::size_t largerStride = static_cast<std::size_t>(kLarger.x) * kBytesPerPixel + 64;
    const auto larger = patternFrame(kLarger, largerStride, 2);
    expect(upload(larger, kLarger, largerStride, gl::VideoUpload::Uploaded, "larger frame"),
           "the larger frame uploads");
    expect(texture.size() == kLarger, "the texture takes the larger frame's size");
    expect(texture.stats().reallocations == 1, "the larger frame reallocates the texture once");
    expect(holds(texture, larger, largerStride), "the texture holds the larger frame (padded rows)");
    const std::size_t largerRing = gl::VideoTexture::kRingSize * alignedSlotBytes(kLarger);
    expect(texture.gpuBytes() == static_cast<std::size_t>(kLarger.x * kLarger.y) * kBytesPerPixel + largerRing,
           fmt::format("the ring grows to {} slots of the larger frame ({} bytes with the texture)",
                       gl::VideoTexture::kRingSize, texture.gpuBytes()));

    // 3. A smaller frame: the texture is reallocated, the ring kept.
    const std::size_t smallerStride = static_cast<std::size_t>(kSmaller.x) * kBytesPerPixel;
    const auto smaller = patternFrame(kSmaller, smallerStride, 3);
    expect(upload(smaller, kSmaller, smallerStride, gl::VideoUpload::Uploaded, "smaller frame"),
           "the smaller frame uploads");
    expect(texture.size() == kSmaller, "the texture takes the smaller frame's size");
    expect(texture.stats().reallocations == 2, "the smaller frame reallocates the texture");
    expect(texture.gpuBytes() == static_cast<std::size_t>(kSmaller.x * kSmaller.y) * kBytesPerPixel + largerRing,
           "the ring is kept for the smaller frame");
    expect(holds(texture, smaller, smallerStride), "the texture holds the smaller frame");

    // 4. Twice the ring's slots of uploads within one frame, queued behind GPU work: the slot of the first comes round
    // again before the GPU has copied from it, and upload() returns RingBusy instead of waiting.
    const gl::TextureObject busyFirst = busyTexture(kBusySide);
    const gl::TextureObject busySecond = busyTexture(kBusySide);
    expect(busyFirst.valid() && busySecond.valid(), "the textures that keep the GPU busy are created");
    for (int burst = 1; burst <= kMaxBursts && result.burstsUntilBusy == 0; ++burst) {
        const std::uint64_t busyBefore = texture.stats().ringBusy;
        occupyGpu(busyFirst, busySecond, kBusySide, kBusyCopies * burst);
        for (std::size_t index = 0; index < 2 * gl::VideoTexture::kRingSize; ++index) {
            const auto frame = patternFrame(kSmaller, smallerStride, static_cast<int>(10 + index));
            const auto uploaded = texture.upload(std::span<const std::uint8_t>(frame), kSmaller, smallerStride);
            if (!uploaded) {
                result.failures.push_back("burst upload: " + uploaded.error().toString());
                break;
            }
        }
        if (texture.stats().ringBusy > busyBefore) {
            result.burstsUntilBusy = burst;
        }
        glFinish();
    }
    expect(result.burstsUntilBusy > 0,
           fmt::format("a burst of uploads finds the ring busy ({} bursts tried)", kMaxBursts));

    // 5. Once the GPU is done (glFinish above), the ring is free again: the next upload goes through.
    const auto last = patternFrame(kSmaller, smallerStride, 42);
    expect(upload(last, kSmaller, smallerStride, gl::VideoUpload::Uploaded, "upload after glFinish"),
           "the ring is free again once the GPU is done");
    expect(holds(texture, last, smallerStride), "the texture holds the frame uploaded last");

    // 6. The uploads' GPU timings arrive.
    glFinish();
    while (texture.pollGpuMilliseconds()) {
        ++result.gpuTimings;
    }
    expect(result.gpuTimings > 0, "the uploads' GPU timings arrive");

    result.uploads = texture.stats().uploads;
    result.reallocations = texture.stats().reallocations;
    result.ringBusy = texture.stats().ringBusy;
    result.passed = result.failures.empty();
    if (result.passed) {
        spdlog::info("VideoTexture self-test passed: {} uploads, {} reallocations, ring busy {} time(s) in burst {}, "
                     "{} GPU timings",
                     result.uploads, result.reallocations, result.ringBusy, result.burstsUntilBusy, result.gpuTimings);
    } else {
        for (const std::string& failure : result.failures) {
            spdlog::error("VideoTexture self-test: {}", failure);
        }
    }
    return result;
}

} // namespace rv::spike
