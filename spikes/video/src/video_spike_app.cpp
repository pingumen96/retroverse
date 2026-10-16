#include "video_spike_app.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

#include <SDL3/SDL_video.h>
#include <fmt/format.h>
#include <glad/gl.h>
#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>
#include <rv/media/test_video.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/render/gl_device.hpp>

namespace rv::spike {
namespace {

/// A quad from uRect (left, bottom, right, top in normalized device coordinates) as a 4-vertex triangle strip without
/// vertex buffers. uv (0, 0) is the top-left corner: texture row 0, the picture's top row (ADR-0002).
constexpr std::string_view kVertexShader = R"(#version 450 core
uniform vec4 uRect;
out vec2 vUv;
void main() {
    vec2 corner = vec2(float(gl_VertexID >> 1), float(gl_VertexID & 1));
    vUv = corner;
    gl_Position = vec4(mix(uRect.x, uRect.z, corner.x), mix(uRect.w, uRect.y, corner.y), 0.0, 1.0);
}
)";

/// Unlit: the picture's linear color (sRGB textures decode on sampling) times a tint, written to an sRGB target that
/// encodes it again, so the screen shows the video's own values.
constexpr std::string_view kFragmentShader = R"(#version 450 core
layout(binding = 0) uniform sampler2D uPicture;
uniform vec4 uTint;
in vec2 vUv;
layout(location = 0) out vec4 oColor;
void main() {
    oColor = vec4(texture(uPicture, vUv).rgb * uTint.rgb, 1.0);
}
)";

constexpr glm::vec4 kBackground{0.012f, 0.012f, 0.014f, 1.0f};
constexpr glm::vec4 kWhite{1.0f};
constexpr glm::vec4 kBlack{0.0f, 0.0f, 0.0f, 1.0f};
constexpr glm::vec4 kPlayingBorder{0.02f, 0.30f, 0.06f, 1.0f};
constexpr glm::vec4 kWaitingBorder{0.10f, 0.10f, 0.10f, 1.0f};
constexpr glm::vec4 kFailedBorder{0.55f, 0.02f, 0.02f, 1.0f};
constexpr float kMarginPixels = 24.0f;
constexpr float kBorderPixels = 4.0f;
/// Frames per second the recorders are sized for (a run without vsync renders thousands).
constexpr double kRecorderFramesPerSecond = 2500.0;
/// Criteria of the roadmap's row (c).
constexpr double kMaxP99FrameMs = 16.7;
/// The render rate the roadmap's criteria are stated for: a 60 FPS loop meets "p99 at most 16.7 ms" when at most 1% of
/// its frames, 0.6 per second, take longer.
constexpr double kCriteriaFramesPerSecond = 60.0;
constexpr double kMaxLongFrameShare = 0.01;
constexpr double kMaxUploadMs = 2.0;
constexpr double kMaxLateOrDroppedPercent = 1.0;
constexpr double kMaxDriftPercent = 2.0;
constexpr double kMaxMemoryGrowthMiB = 16.0;

double milliseconds(std::chrono::steady_clock::duration duration) noexcept {
    return std::chrono::duration<double, std::milli>(duration).count();
}

/// `value` rounded to a multiple of `step` (a power of ten below 1), so that the report reads 6.18,
/// not 6.180000000000001.
double rounded(double value, double step = 1e-4) {
    const double scale = std::round(1.0 / step);
    return std::isfinite(value) ? std::round(value * scale) / scale : 0.0;
}

nlohmann::json summaryJson(const media::SampleSummary& summary) {
    return nlohmann::json{{"count", summary.count},      {"mean", rounded(summary.mean)}, {"min", rounded(summary.min)},
                          {"p50", rounded(summary.p50)}, {"p95", rounded(summary.p95)},   {"p99", rounded(summary.p99)},
                          {"max", rounded(summary.max)}};
}

/// The pixel rectangle (x0, y0, x1, y1 from the top-left) in normalized device coordinates (left, bottom, right, top).
glm::vec4 toNdc(const glm::vec4& pixels, glm::vec2 window) {
    return {pixels.x / window.x * 2.0f - 1.0f, 1.0f - pixels.w / window.y * 2.0f, pixels.z / window.x * 2.0f - 1.0f,
            1.0f - pixels.y / window.y * 2.0f};
}

/// The pixel rectangle (x0, y0, x1, y1 from the top-left) of picture `index` of `count` in a `window`: a grid of
/// cells, the picture of `pictureSize` letterboxed in its cell inside a border. Nothing when the window is too small.
std::optional<glm::vec4> pictureRect(int index, int count, glm::vec2 window, glm::ivec2 pictureSize) {
    const int columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))));
    const int rows = std::max(1, (count + columns - 1) / columns);
    const glm::vec2 cell((window.x - kMarginPixels * static_cast<float>(columns + 1)) / static_cast<float>(columns),
                         (window.y - kMarginPixels * static_cast<float>(rows + 1)) / static_cast<float>(rows));
    if (cell.x <= 2.0f * kBorderPixels || cell.y <= 2.0f * kBorderPixels) {
        return std::nullopt;
    }
    const float aspect = static_cast<float>(pictureSize.x) / static_cast<float>(std::max(pictureSize.y, 1));
    const glm::vec2 inner = cell - 2.0f * kBorderPixels;
    const glm::vec2 fitted =
        inner.x / inner.y > aspect ? glm::vec2(inner.y * aspect, inner.y) : glm::vec2(inner.x, inner.x / aspect);
    const glm::vec2 cellMin(kMarginPixels + static_cast<float>(index % columns) * (cell.x + kMarginPixels),
                            kMarginPixels + static_cast<float>(index / columns) * (cell.y + kMarginPixels));
    const glm::vec2 pictureMin = cellMin + (cell - fitted) * 0.5f;
    return glm::vec4(pictureMin, pictureMin + fitted);
}

/// Percent `part` of `whole`, 0 for an empty whole.
double percentOf(double part, double whole) noexcept {
    return whole > 0.0 ? 100.0 * part / whole : 0.0;
}

} // namespace

VideoSpikeApp::VideoSpikeApp(SpikeOptions options, std::vector<std::filesystem::path> videos)
    : m_options(std::move(options)) {
    m_slots.reserve(videos.size());
    for (std::size_t index = 0; index < videos.size(); ++index) {
        Slot slot;
        slot.path = std::move(videos[index]);
        slot.name = fmt::format("v{} {}", index, core::pathToUtf8(slot.path.filename()));
        m_slots.push_back(std::move(slot));
    }
}

VideoSpikeApp::~VideoSpikeApp() = default;

Status VideoSpikeApp::onInit(app::AppContext& context) {
    auto program = gl::ShaderProgram::build(kVertexShader, kFragmentShader, "spike video quad");
    if (!program) {
        return propagate(program.error());
    }
    m_program = std::move(*program);
    auto quad = gl::VertexArray::create("spike video quads");
    if (!quad) {
        return propagate(quad.error());
    }
    m_quad = std::move(*quad);
    gl::TextureDesc whiteDesc;
    whiteDesc.mipmaps = false;
    auto white = gl::Texture2D::create(assets::makeSolid(1, 1, glm::u8vec4(255)), whiteDesc, "spike white");
    if (!white) {
        return propagate(white.error());
    }
    m_white = std::move(*white);
    if (const SDL_DisplayID display = SDL_GetDisplayForWindow(context.window().handle()); display != 0) {
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display); mode != nullptr) {
            m_displayRefreshHz = mode->refresh_rate;
        }
    }
    if (m_options.textureSelfTest) {
        m_textureSelfTest = runTextureSelfTest();
    }

    const auto capacity = static_cast<std::size_t>((m_options.seconds + 5.0) * kRecorderFramesPerSecond);
    m_frameIntervals = media::SampleRecorder(capacity);
    m_frameWork = media::SampleRecorder(capacity);
    m_uploadCpu = media::SampleRecorder(capacity);
    m_uploadCopy = media::SampleRecorder(capacity);
    m_uploadGpu = media::SampleRecorder(capacity);
    m_uploadPerFrame = media::SampleRecorder(capacity);
    m_machineBusy = media::SampleRecorder(static_cast<std::size_t>(m_options.seconds) + 8);
    m_memory.reserve(static_cast<std::size_t>(m_options.seconds) + 8);

    if (!setProcessPriority(m_options.priority)) {
        spdlog::warn("Process priority '{}' refused; running at the default priority", m_options.priority);
    }
    spdlog::info("{}", media::ffmpegVersionInfo());
    spdlog::info("Video spike: {} video(s) for {} s after a {} s warm-up, window {}x{}, vsync {}, display {:.0f} Hz, "
                 "frames up to {} rows, queue of {}, {} decoder thread(s) each",
                 m_slots.size(), m_options.seconds, m_options.warmupSeconds, context.framebufferSize().x,
                 context.framebufferSize().y, m_options.host.vsync ? "on" : "off", m_displayRefreshHz,
                 m_options.maxHeight, m_options.queueCapacity, m_options.decoderThreads);
    for (Slot& slot : m_slots) {
        media::DecoderSettings settings;
        settings.maxHeight = m_options.maxHeight;
        settings.queueCapacity = m_options.queueCapacity;
        settings.threadCount = m_options.decoderThreads;
        settings.loop = true;
        settings.name = slot.name;
        slot.startedAt = Clock::now();
        auto decoder = media::VideoDecoder::start(slot.path, settings);
        if (!decoder) {
            slot.startError = decoder.error().toString();
            spdlog::warn("Video '{}' not started: {}", slot.name, *slot.startError);
            continue;
        }
        slot.playback = std::make_unique<media::VideoPlayback>(std::move(*decoder));
    }
    m_memoryAtInit = sampleMemory();
    return ok();
}

void VideoSpikeApp::startMeasuring(double now) {
    m_measuring = true;
    m_measureStart = now;
    m_nextSample = now + 1.0;
    m_cpuAtStart = sampleCpu();
    m_cpuLastSample = m_cpuAtStart;
    m_memory.push_back(sampleMemory());
    for (Slot& slot : m_slots) {
        if (slot.playback) {
            slot.clockAtStart = slot.playback->clock().stats();
            slot.cpuAtStart = slot.playback->decoder().threadCpuSeconds();
        }
        slot.uploadsAtStart = slot.uploads;
    }
    spdlog::info("Warm-up over after {} frame(s): measuring for {} s", m_frames, m_options.seconds);
}

void VideoSpikeApp::finishMeasuring(double now) {
    m_measuring = false;
    m_finished = true;
    m_measureEnd = now;
    m_cpuAtEnd = sampleCpu();
    m_memory.push_back(sampleMemory());
    for (Slot& slot : m_slots) {
        if (slot.playback) {
            slot.clockAtEnd = slot.playback->clock().stats();
            slot.cpuAtEnd = slot.playback->decoder().threadCpuSeconds();
        }
        slot.uploadsAtEnd = slot.uploads;
    }
    spdlog::info("Measured {} frame(s) in {:.2f} s", m_framesMeasured, m_measureEnd - m_measureStart);
}

void VideoSpikeApp::sampleSecond(double now) {
    const CpuTimes cpu = sampleCpu();
    m_machineBusy.record(cpuLoadBetween(m_cpuLastSample, cpu).machineBusyPercent);
    m_cpuLastSample = cpu;
    if (m_memory.size() < m_memory.capacity()) {
        m_memory.push_back(sampleMemory());
    }
    m_nextSample = now + 1.0;
}

void VideoSpikeApp::showFallback(Slot& slot, double now) {
    slot.failedAtSeconds = now;
    std::string reason = "cannot start";
    if (slot.playback) {
        reason = std::string(media::toString(slot.playback->decoder().failure()));
        if (const auto error = slot.playback->decoder().error()) {
            spdlog::warn("Video '{}' falls back to a still picture at {:.3f} s: {}", slot.name, now, error->toString());
        }
    }
    gl::TextureDesc desc;
    desc.mipmaps = false;
    const assets::ImageData picture = assets::makeCoverPlaceholder(480, 360, "NO VIDEO: " + reason, 7);
    auto fallback = gl::Texture2D::create(picture, desc, slot.name + " fallback");
    if (fallback) {
        slot.fallback = std::move(*fallback);
    } else {
        spdlog::warn("Video '{}': no fallback picture: {}", slot.name, fallback.error().toString());
    }
}

void VideoSpikeApp::updateSlot(Slot& slot, double now) {
    if (!slot.playback) {
        if (!slot.failedAtSeconds) {
            showFallback(slot, now);
        }
        return;
    }
    const media::PlaybackUpdate update = slot.playback->update(now);
    if (update.clock.newFrame && !slot.firstShownAt) {
        slot.firstShownAt = m_frameStart;
    }
    if (update.clock.newFrame && update.clock.shown && m_measuring) {
        const glm::dvec2 shown(now, update.clock.shown->ptsSeconds);
        if (!slot.firstShown) {
            slot.firstShown = shown;
        }
        slot.lastShown = shown;
    }
    // The still picture replaces the video once the frames decoded before the failure have been shown (ADR-0004).
    if (slot.playback->failed() && !slot.failedAtSeconds) {
        showFallback(slot, now);
    }
    if (update.frame == nullptr || slot.textureFailed) {
        return;
    }
    const media::VideoFrame& frame = *update.frame;
    const glm::ivec2 size(frame.width(), frame.height());
    if (!slot.texture.valid()) {
        auto texture = gl::VideoTexture::create(
            size, slot.name, m_options.uploadFences ? gl::VideoUploadSync::Fence : gl::VideoUploadSync::TimestampQuery);
        if (!texture) {
            spdlog::error("Video '{}': no texture: {}", slot.name, texture.error().toString());
            slot.textureFailed = true;
            return;
        }
        slot.texture = std::move(*texture);
    }
    const Clock::time_point start = Clock::now();
    const auto uploaded = slot.texture.upload(frame.bytes(), size, frame.strideBytes());
    const double uploadMs = milliseconds(Clock::now() - start);
    if (!uploaded) {
        if (++slot.uploadErrors == 1) {
            spdlog::error("Video '{}': upload failed: {}", slot.name, uploaded.error().toString());
        }
        slot.playback->releaseFrame();
        return;
    }
    if (*uploaded == gl::VideoUpload::Uploaded) {
        if (m_options.checkFrames) {
            slot.lastUploadedCounter = media::decodeFrameCounter(frame); // before the frame goes back to the pool
        }
        slot.playback->releaseFrame();
        ++slot.uploads;
        m_frameUploadMs += uploadMs;
        ++m_frameUploads;
        if (m_measuring) {
            m_uploadCpu.record(uploadMs);
            m_uploadCopy.record(slot.texture.stats().lastCopyMilliseconds);
        }
    }
}

void VideoSpikeApp::onUpdate(app::AppContext& context, double /*deltaSeconds*/) {
    m_frameStart = Clock::now();
    if (!m_runStart) {
        m_runStart = m_frameStart;
    }
    m_now = std::chrono::duration<double>(m_frameStart - *m_runStart).count();
    if (m_measuring && m_lastFrameStart) {
        const double interval = milliseconds(m_frameStart - *m_lastFrameStart);
        m_frameIntervals.record(interval);
        if (interval > kMaxP99FrameMs) {
            ++m_longFrames;
        }
    }
    m_lastFrameStart = m_frameStart;
    if (!m_measuring && !m_finished && m_now >= m_options.warmupSeconds) {
        startMeasuring(m_now);
    }

    m_frameUploadMs = 0.0;
    m_frameUploads = 0;
    for (Slot& slot : m_slots) {
        updateSlot(slot, m_now);
        while (const std::optional<double> gpu = slot.texture.pollGpuMilliseconds()) {
            if (m_measuring) {
                m_uploadGpu.record(*gpu);
            }
        }
    }
    if (m_measuring && m_frameUploads > 0) {
        m_uploadPerFrame.record(m_frameUploadMs);
        ++m_framesWithUploads;
        if (m_frameUploads > 1) {
            ++m_framesWithSeveralUploads;
        }
    }
    if (m_measuring && m_now >= m_nextSample) {
        sampleSecond(m_now);
    }
    if (m_measuring && m_now >= m_options.warmupSeconds + m_options.seconds) {
        finishMeasuring(m_now);
        context.requestQuit(); // this frame is still rendered (and captured)
    }
}

void VideoSpikeApp::drawSlots(app::AppContext& context) {
    gl::GLDevice& device = context.device();
    const glm::vec2 window(context.framebufferSize());
    device.state().setDepthTest(false);
    device.state().setCullFace(false);
    device.state().setBlend(false);
    m_program.use();
    m_quad.bind();
    const auto draw = [&](const glm::vec4& pixels, GLuint texture, const glm::vec4& tint) {
        m_program.setVec4("uRect", toNdc(pixels, window));
        m_program.setVec4("uTint", tint);
        glBindTextureUnit(0, texture);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };

    const int count = static_cast<int>(m_slots.size());
    for (int index = 0; index < count; ++index) {
        const Slot& slot = m_slots[static_cast<std::size_t>(index)];
        GLuint picture = m_white.id();
        glm::vec4 tint = kBlack;
        glm::vec4 border = kWaitingBorder;
        glm::ivec2 pictureSize(16, 9);
        if (slot.failedAtSeconds) {
            border = kFailedBorder;
            if (slot.fallback.valid()) {
                picture = slot.fallback.id();
                pictureSize = slot.fallback.size();
                tint = kWhite;
            }
        } else if (slot.texture.valid()) {
            border = kPlayingBorder;
            picture = slot.texture.textureId();
            pictureSize = slot.texture.size();
            tint = kWhite;
        }
        const std::optional<glm::vec4> rect = pictureRect(index, count, window, pictureSize);
        if (!rect) {
            return;
        }
        draw(glm::vec4(rect->x - kBorderPixels, rect->y - kBorderPixels, rect->z + kBorderPixels,
                       rect->w + kBorderPixels),
             m_white.id(), border);
        draw(*rect, picture, tint);
    }
}

void VideoSpikeApp::onRender(app::AppContext& context) {
    const glm::ivec2 size = context.framebufferSize();
    if (size.x > 0 && size.y > 0) {
        if (!m_target.valid()) {
            gl::FramebufferDesc desc;
            desc.size = size;
            desc.depthStencil = false;
            if (auto target = gl::Framebuffer::create(desc, "spike video output"); target) {
                m_target = std::move(*target);
            } else {
                spdlog::error("No output target: {}", target.error().toString());
            }
        } else if (m_target.size() != size) {
            if (auto resized = m_target.resize(size); !resized) {
                spdlog::error("Output target not resized: {}", resized.error().toString());
            }
        }
        if (m_target.valid()) {
            context.device().beginPass(m_target, kBackground);
            drawSlots(context);
            m_target.blitToDefault(context.device().state(), size);
        }
    }
    ++m_frames;
    if (m_measuring) {
        ++m_framesMeasured;
        m_frameWork.record(milliseconds(Clock::now() - m_frameStart));
    }
    if (m_finished && m_options.screenshot && !m_screenshotWritten && !m_screenshotError) {
        writeScreenshot(context);
    }
}

void VideoSpikeApp::readScreenCounters(const assets::ImageData& image) {
    if (image.channels != 4 || !image.isValid()) {
        return;
    }
    const glm::vec2 window(static_cast<float>(image.width), static_cast<float>(image.height));
    const int count = static_cast<int>(m_slots.size());
    for (int index = 0; index < count; ++index) {
        Slot& slot = m_slots[static_cast<std::size_t>(index)];
        if (slot.failedAtSeconds || !slot.texture.valid()) {
            continue;
        }
        const std::optional<glm::vec4> rect = pictureRect(index, count, window, slot.texture.size());
        if (!rect) {
            continue;
        }
        const int x0 = std::clamp(static_cast<int>(std::lround(rect->x)), 0, image.width);
        const int y0 = std::clamp(static_cast<int>(std::lround(rect->y)), 0, image.height);
        const int x1 = std::clamp(static_cast<int>(std::lround(rect->z)), 0, image.width);
        const int y1 = std::clamp(static_cast<int>(std::lround(rect->w)), 0, image.height);
        media::VideoFrame picture;
        if (!picture.resize(media::FrameSize{x1 - x0, y1 - y0})) {
            continue;
        }
        double luma = 0.0;
        for (int y = y0; y < y1; ++y) {
            const std::uint8_t* source = image.pixels.data() + static_cast<std::size_t>(y) * image.rowBytes() +
                                         static_cast<std::size_t>(x0) * 4u;
            std::memcpy(picture.pixels() + static_cast<std::size_t>(y - y0) * picture.strideBytes(), source,
                        picture.strideBytes());
            for (int x = x0; x < x1; ++x, source += 4) {
                luma += 0.299 * source[0] + 0.587 * source[1] + 0.114 * source[2];
            }
        }
        slot.screenCounter = media::decodeFrameCounter(picture);
        slot.screenMeanLuma = luma / (static_cast<double>(x1 - x0) * static_cast<double>(y1 - y0));
    }
}

void VideoSpikeApp::writeScreenshot(app::AppContext& context) {
    const std::filesystem::path& path = *m_options.screenshot;
    auto pixels = m_target.valid()
                      ? m_target.readPixels()
                      : Expected<assets::ImageData>(makeError(ErrorCode::GraphicsError, "no output target"));
    if (pixels && m_options.checkFrames) {
        readScreenCounters(*pixels);
    }
    Status written = pixels ? assets::writePng(path, assets::convertChannels(*pixels, 3)) : propagate(pixels.error());
    if (!written) {
        m_screenshotError = written.error().toString();
        context.failRun(
            Error{ErrorCode::IoError, "screenshot not written: " + *m_screenshotError, core::pathToUtf8(path)});
        return;
    }
    m_screenshotWritten = true;
    spdlog::info("Screenshot written: {}", core::pathToUtf8(path));
}

nlohmann::json VideoSpikeApp::buildReport(app::AppContext& context) const {
    const double window = std::max(m_measureEnd - m_measureStart, 1e-9);
    const CpuLoad load = cpuLoadBetween(m_cpuAtStart, m_cpuAtEnd);
    const gl::GLCapabilities& caps = context.device().caps();

    nlohmann::json report;
    report["spike"] = "video";
    report["version"] = std::string(core::kVersion);
    report["buildType"] = std::string(core::kBuildType);
    report["ffmpeg"] = media::ffmpegVersionInfo();
    report["machine"] = {{"cpu", cpuName()},          {"logicalProcessors", logicalProcessorCount()},
                         {"gpu", caps.renderer},      {"glVendor", caps.vendor},
                         {"glVersion", caps.version}, {"displayRefreshHz", rounded(m_displayRefreshHz, 0.01)}};
    report["settings"] = {
        {"videos", m_slots.size()},
        {"seconds", m_options.seconds},
        {"warmupSeconds", m_options.warmupSeconds},
        {"window", {context.framebufferSize().x, context.framebufferSize().y}},
        {"vsync", m_options.host.vsync},
        {"vsyncMode", std::string(platform::toString(context.window().vsync()))},
        {"hidden", m_options.host.hidden},
        {"maxHeight", m_options.maxHeight},
        {"queueCapacity", m_options.queueCapacity},
        {"decoderThreads", m_options.decoderThreads},
        {"priority", m_options.priority},
        {"uploadSync", std::string(gl::toString(m_options.uploadFences ? gl::VideoUploadSync::Fence
                                                                       : gl::VideoUploadSync::TimestampQuery))}};
    const gl::GLDevice& device = context.device();
    report["glDebugMessages"] = {{"enabled", device.debugOutputEnabled()},
                                 {"high", device.debugMessageCount(gl::DebugSeverity::High)},
                                 {"mediumOrHigher", device.debugMessageCount(gl::DebugSeverity::Medium)},
                                 {"lowOrHigher", device.debugMessageCount(gl::DebugSeverity::Low)}};
    if (m_textureSelfTest) {
        report["textureSelfTest"] = m_textureSelfTest->toJson();
    }
    report["run"] = {{"completed", m_finished},
                     {"framesRendered", m_frames},
                     {"framesMeasured", m_framesMeasured},
                     {"measuredSeconds", rounded(m_finished ? window : 0.0)},
                     {"framesPerSecond", rounded(m_finished ? static_cast<double>(m_framesMeasured) / window : 0.0)},
                     {"screenshot", m_screenshotWritten ? core::pathToUtf8(*m_options.screenshot) : std::string()}};
    report["frameIntervalMs"] = summaryJson(m_frameIntervals.summary());
    // The p99 criterion read at the roadmap's 60 FPS: the intervals over 16.7 ms against 1% of the frames a 60 FPS loop
    // renders in the window (a stall makes one long interval at any rate).
    const double longFramesAllowed = kMaxLongFrameShare * kCriteriaFramesPerSecond * window;
    report["framesOver16_7Ms"] = {
        {"count", m_longFrames},
        {"percentOf60HzFrames",
         rounded(percentOf(static_cast<double>(m_longFrames), kCriteriaFramesPerSecond * window), 0.001)},
        {"allowedAt60Hz", rounded(longFramesAllowed, 0.1)}};
    report["frameWorkMs"] = summaryJson(m_frameWork.summary());
    report["uploadCpuMs"] = summaryJson(m_uploadCpu.summary());
    report["uploadCopyCpuMs"] = summaryJson(m_uploadCopy.summary());
    report["uploadGpuMs"] = summaryJson(m_uploadGpu.summary());
    // Every upload of a render frame together, over the frames that uploaded (videos due on the same frame add up).
    const media::SampleSummary uploadPerFrame = m_uploadPerFrame.summary();
    report["uploadPerFrameCpuMs"] = summaryJson(uploadPerFrame);
    report["framesWithUploads"] = m_framesWithUploads;
    report["framesWithSeveralUploads"] = m_framesWithSeveralUploads;
    report["samplesNotStored"] =
        m_frameIntervals.overflow() + m_frameWork.overflow() + m_uploadCpu.overflow() + m_uploadPerFrame.overflow();

    nlohmann::json memory;
    memory["workingSetAtInitMiB"] = rounded(m_memoryAtInit.workingSetMiB, 0.01);
    if (!m_memory.empty()) {
        double low = m_memory.front().workingSetMiB;
        double high = low;
        for (const MemorySample& sample : m_memory) {
            low = std::min(low, sample.workingSetMiB);
            high = std::max(high, sample.workingSetMiB);
        }
        memory["workingSetStartMiB"] = rounded(m_memory.front().workingSetMiB, 0.01);
        memory["workingSetEndMiB"] = rounded(m_memory.back().workingSetMiB, 0.01);
        memory["workingSetMinMiB"] = rounded(low, 0.01);
        memory["workingSetMaxMiB"] = rounded(high, 0.01);
        memory["workingSetGrowthMiB"] = rounded(m_memory.back().workingSetMiB - m_memory.front().workingSetMiB, 0.01);
        memory["privateStartMiB"] = rounded(m_memory.front().privateMiB, 0.01);
        memory["privateEndMiB"] = rounded(m_memory.back().privateMiB, 0.01);
        memory["privateGrowthMiB"] = rounded(m_memory.back().privateMiB - m_memory.front().privateMiB, 0.01);
        memory["peakWorkingSetMiB"] = rounded(m_memory.back().peakWorkingSetMiB, 0.01);
        memory["samples"] = m_memory.size();
        // One sample per second of the window (the first and the last at its ends): the trend, not only the ends.
        nlohmann::json workingSet = nlohmann::json::array();
        nlohmann::json privateBytes = nlohmann::json::array();
        for (const MemorySample& sample : m_memory) {
            workingSet.push_back(rounded(sample.workingSetMiB, 0.01));
            privateBytes.push_back(rounded(sample.privateMiB, 0.01));
        }
        memory["workingSetMiBPerSecond"] = std::move(workingSet);
        memory["privateMiBPerSecond"] = std::move(privateBytes);
    }
    report["memory"] = memory;
    report["load"] = {{"machineBusyPercent", rounded(load.machineBusyPercent, 0.01)},
                      {"machineBusyPerSecond", summaryJson(m_machineBusy.summary())},
                      {"processPercentOfOneCore", rounded(load.processPercent, 0.01)},
                      {"mainThreadPercentOfOneCore", rounded(load.mainThreadPercent, 0.01)}};

    bool everyPlayingVideoOnTime = true;
    bool everyPlayingVideoWithinDrift = true;
    int playing = 0;
    nlohmann::json videos = nlohmann::json::array();
    for (const Slot& slot : m_slots) {
        nlohmann::json video;
        video["name"] = slot.name;
        video["file"] = core::pathToUtf8(slot.path);
        video["fallback"] = slot.failedAtSeconds.has_value();
        video["fallbackAtSeconds"] = slot.failedAtSeconds ? rounded(*slot.failedAtSeconds) : -1.0;
        if (!slot.playback) {
            video["state"] = "not started";
            video["failure"] = slot.startError.value_or("");
            videos.push_back(video);
            continue;
        }
        const media::VideoDecoder& decoder = slot.playback->decoder();
        video["state"] = std::string(media::toString(decoder.state()));
        video["failure"] = std::string(media::toString(decoder.failure()));
        const auto error = decoder.error();
        video["error"] = error ? error->message : std::string();
        if (const auto info = decoder.streamInfo()) {
            video["stream"] = {{"container", info->container},
                               {"codec", info->codec},
                               {"pixelFormat", info->pixelFormat},
                               {"codedSize", {info->codedSize.width, info->codedSize.height}},
                               {"outputSize", {info->outputSize.width, info->outputSize.height}},
                               {"frameRate", rounded(info->frameRate)},
                               {"durationSeconds", rounded(info->durationSeconds)},
                               {"bitRate", info->bitRate}};
        }
        const media::DecoderStats stats = decoder.stats();
        video["startup"] = {{"openMs", rounded(stats.openMilliseconds, 0.01)},
                            {"firstFrameQueuedMs", rounded(stats.firstFrameMilliseconds, 0.01)},
                            {"firstFrameShownMs", slot.firstShownAt
                                                      ? rounded(milliseconds(*slot.firstShownAt - slot.startedAt), 0.01)
                                                      : -1.0}};
        video["decoder"] = {
            {"framesDecoded", stats.framesDecoded},
            {"framesQueued", stats.framesQueued},
            {"decodeErrors", stats.decodeErrors},
            {"damagedFrames", stats.damagedFrames},
            {"corruptPackets", stats.corruptPackets},
            {"loops", stats.loops},
            {"frameAllocations", stats.frameAllocations},
            {"queueWaitSeconds", rounded(stats.queueWaitSeconds)},
            {"decodeMs", summaryJson(stats.decodeMilliseconds)},
            {"convertMs", summaryJson(stats.convertMilliseconds)},
            {"threadCpuSeconds", rounded(slot.cpuAtEnd - slot.cpuAtStart)},
            {"threadCpuPercentOfOneCore", rounded(100.0 * (slot.cpuAtEnd - slot.cpuAtStart) / window, 0.01)}};
        const media::PresentationStats& end = slot.clockAtEnd;
        const media::PresentationStats& start = slot.clockAtStart;
        const auto presented = static_cast<double>(end.presented - start.presented);
        const auto late = static_cast<double>(end.late - start.late);
        const auto dropped = static_cast<double>(end.dropped - start.dropped);
        // A dropped frame was never shown: not on time either. The criterion counts both against every frame that
        // fell due.
        const double lateOrDroppedPercent = percentOf(late + dropped, presented + dropped);
        video["presentation"] = {
            {"presented", end.presented - start.presented},
            {"late", end.late - start.late},
            {"latePercent", rounded(percentOf(late, presented))},
            {"dropped", end.dropped - start.dropped},
            {"droppedPercent", rounded(percentOf(dropped, presented + dropped))},
            {"lateOrDroppedPercent", rounded(lateOrDroppedPercent)},
            {"repeated", end.repeated - start.repeated},
            {"resyncs", end.resyncs - start.resyncs},
            {"resyncSeconds", rounded(end.resyncSeconds - start.resyncSeconds)},
            {"stale", end.stale - start.stale},
            {"maxLatenessMs", rounded(end.maxLatenessSeconds * 1000.0)},
            {"meanLatenessMs",
             rounded(presented > 0.0 ? 1000.0 * (end.totalLatenessSeconds - start.totalLatenessSeconds) / presented
                                     : 0.0)},
            {"presentedPerSecond", rounded(presented / window)},
            {"supersededBeforeUpload", slot.playback->supersededFrames()}};
        double driftPercent = 0.0;
        if (slot.firstShown) {
            const double wallSpan = slot.lastShown.x - slot.firstShown->x;
            const double ptsSpan = slot.lastShown.y - slot.firstShown->y;
            driftPercent = wallSpan > 0.0 ? 100.0 * (ptsSpan - wallSpan) / wallSpan : 0.0;
            video["drift"] = {{"wallSeconds", rounded(wallSpan)},
                              {"videoSeconds", rounded(ptsSpan)},
                              {"percent", rounded(driftPercent)}};
        }
        nlohmann::json upload = {{"uploads", slot.uploadsAtEnd - slot.uploadsAtStart},
                                 {"sync", std::string(gl::toString(slot.texture.sync()))},
                                 {"ringBusy", slot.texture.stats().ringBusy},
                                 {"reallocations", slot.texture.stats().reallocations},
                                 {"gpuTimingsLost", slot.texture.stats().gpuTimingsLost},
                                 {"errors", slot.uploadErrors},
                                 {"textureSize", {slot.texture.size().x, slot.texture.size().y}},
                                 {"gpuBytes", slot.texture.gpuBytes()}};
        if (m_options.checkFrames) {
            // -1: no counter could be read (not a test clip, or a picture that is not a test frame).
            upload["lastCounter"] =
                slot.lastUploadedCounter ? static_cast<std::int64_t>(*slot.lastUploadedCounter) : -1;
            upload["screenCounter"] = slot.screenCounter ? static_cast<std::int64_t>(*slot.screenCounter) : -1;
            upload["screenMeanLuma"] = rounded(slot.screenMeanLuma.value_or(-1.0), 0.1);
        }
        video["upload"] = std::move(upload);
        if (!slot.failedAtSeconds) {
            ++playing;
            everyPlayingVideoOnTime = everyPlayingVideoOnTime && lateOrDroppedPercent < kMaxLateOrDroppedPercent;
            everyPlayingVideoWithinDrift = everyPlayingVideoWithinDrift && std::abs(driftPercent) <= kMaxDriftPercent;
        }
        videos.push_back(video);
    }
    report["videos"] = videos;

    const media::SampleSummary frames = m_frameIntervals.summary();
    const media::SampleSummary uploadCpu = m_uploadCpu.summary();
    const media::SampleSummary uploadGpu = m_uploadGpu.summary();
    const double growth = m_memory.empty() ? 0.0 : m_memory.back().workingSetMiB - m_memory.front().workingSetMiB;
    nlohmann::json criteria;
    criteria["frameIntervalP99AtMost16_7Ms"] = m_finished && frames.p99 <= kMaxP99FrameMs;
    criteria["framesOver16_7MsWithin1PercentOf60Hz"] =
        m_finished && static_cast<double>(m_longFrames) <= longFramesAllowed;
    criteria["uploadCpuP99Under2Ms"] = m_finished && uploadCpu.p99 < kMaxUploadMs;
    criteria["uploadGpuP99Under2Ms"] = m_finished && uploadGpu.p99 < kMaxUploadMs;
    criteria["lateOrDroppedUnder1Percent"] = m_finished && playing > 0 && everyPlayingVideoOnTime;
    criteria["driftWithin2Percent"] = m_finished && playing > 0 && everyPlayingVideoWithinDrift;
    criteria["memoryGrowthAtMost16MiB"] = m_finished && growth <= kMaxMemoryGrowthMiB;
    criteria["completed"] = m_finished;
    bool all = true;
    for (const auto& [name, value] : criteria.items()) {
        all = all && value.get<bool>();
    }
    criteria["all"] = all;
    criteria["playingVideos"] = playing;
    report["criteria"] = criteria;
    // The upload budget read per render frame instead of per upload (not a criterion of the report: ADR-0004).
    report["uploadPerFrameCpuP99Under2Ms"] = m_finished && uploadPerFrame.p99 < kMaxUploadMs;
    return report;
}

void VideoSpikeApp::onShutdown(app::AppContext& context) {
    if (!m_finished && m_measuring) {
        finishMeasuring(m_now); // interrupted: report what was measured
        m_finished = false;
    }
    if (m_options.report) {
        try {
            const nlohmann::json report = buildReport(context);
            if (auto written = core::writeTextFileAtomic(*m_options.report, report.dump(2) + "\n"); !written) {
                context.failRun(written.error());
            } else {
                spdlog::info("Report written: {}", core::pathToUtf8(*m_options.report));
            }
        } catch (const std::exception& e) {
            context.failRun(Error{ErrorCode::Unknown, fmt::format("report not written: {}", e.what()),
                                  core::pathToUtf8(*m_options.report)});
        }
    }
    if (m_options.screenshot && !m_screenshotWritten && !m_screenshotError) {
        context.failRun(Error{ErrorCode::IoError, "the run ended before its last frame: no screenshot",
                              core::pathToUtf8(*m_options.screenshot)});
    }
    if (m_textureSelfTest && !m_textureSelfTest->passed) {
        context.failRun(Error{ErrorCode::GraphicsError,
                              fmt::format("VideoTexture self-test failed: {}", m_textureSelfTest->failures.front())});
    }
    // Decoders stop without waiting; their threads are waited for once, here, before the GPU resources go (the
    // context is still current).
    for (Slot& slot : m_slots) {
        if (slot.playback) {
            slot.playback->decoder().stop();
        }
    }
    m_slots.clear();
    media::waitForStoppedDecoders();
    m_target = {};
    m_white = {};
    m_quad = {};
    m_program = {};
}

} // namespace rv::spike
