#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/app_logic.hpp>
#include <rv/assets/image_data.hpp>
#include <rv/media/presentation_clock.hpp>
#include <rv/media/sample_stats.hpp>
#include <rv/media/video_playback.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/shader_program.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/vertex_array.hpp>
#include <rv/render/video_texture.hpp>

#include "spike_options.hpp"
#include "system_metrics.hpp"
#include "texture_self_test.hpp"

namespace rv::spike {

/// The video spike on app::AppHost: plays the videos on textured quads in a grid (rv::media::VideoPlayback for the
/// decoding and the clock, gl::VideoTexture for the upload, an unlit sRGB-correct shader), measures every frame after
/// the warm-up (frame interval and CPU work, intervals over 16.7 ms, upload CPU time per upload and per render frame,
/// upload GPU time, the clock's late, dropped and repeated frames, the decoder threads' CPU time, each video's startup
/// delays, the drift of the video clock against the wall clock, memory, machine load), quits after
/// SpikeOptions::seconds and writes the JSON report and the screenshot of the last frame. A video that cannot play
/// shows a still picture naming why (the fallback of spec §9.5 and SC-08) once the frames decoded before its failure
/// have been shown. Main thread only.
class VideoSpikeApp final : public app::IAppLogic {
public:
    VideoSpikeApp(SpikeOptions options, std::vector<std::filesystem::path> videos);
    ~VideoSpikeApp() override;
    VideoSpikeApp(const VideoSpikeApp&) = delete;
    VideoSpikeApp& operator=(const VideoSpikeApp&) = delete;

    [[nodiscard]] Status onInit(app::AppContext& context) override;
    void onUpdate(app::AppContext& context, double deltaSeconds) override;
    void onRender(app::AppContext& context) override;
    void onShutdown(app::AppContext& context) override;

private:
    using Clock = std::chrono::steady_clock;

    /// One video on screen.
    struct Slot {
        std::filesystem::path path;
        std::string name;
        std::unique_ptr<media::VideoPlayback> playback;
        std::optional<std::string> startError;
        gl::VideoTexture texture;
        bool textureFailed = false;
        gl::Texture2D fallback;
        std::optional<double> failedAtSeconds;
        std::uint64_t uploads = 0;
        std::uint64_t uploadErrors = 0;
        /// When the decoder was started, and when its first frame was shown (NFR-06).
        Clock::time_point startedAt;
        std::optional<Clock::time_point> firstShownAt;
        /// --check-frames: the counter of the frame uploaded last, and the one read from the screenshot's picture
        /// with the picture's mean luma (a black picture reads as counter 0).
        std::optional<std::uint32_t> lastUploadedCounter;
        std::optional<std::uint32_t> screenCounter;
        std::optional<double> screenMeanLuma;

        // The measured window.
        media::PresentationStats clockAtStart;
        media::PresentationStats clockAtEnd;
        std::uint64_t uploadsAtStart = 0;
        std::uint64_t uploadsAtEnd = 0;
        double cpuAtStart = 0.0;
        double cpuAtEnd = 0.0;
        /// First and last frame shown in the window: wall time, timestamp.
        std::optional<glm::dvec2> firstShown;
        glm::dvec2 lastShown{0.0};
    };

    void startMeasuring(double now);
    void finishMeasuring(double now);
    void updateSlot(Slot& slot, double now);
    void showFallback(Slot& slot, double now);
    void sampleSecond(double now);
    void drawSlots(app::AppContext& context);
    void writeScreenshot(app::AppContext& context);
    /// --check-frames: reads the frame counter of every playing video's picture in `image` (the output, top row first).
    void readScreenCounters(const assets::ImageData& image);
    [[nodiscard]] nlohmann::json buildReport(app::AppContext& context) const;

    SpikeOptions m_options;
    std::vector<Slot> m_slots;

    gl::ShaderProgram m_program;
    gl::VertexArray m_quad;
    gl::Framebuffer m_target;
    gl::Texture2D m_white;
    float m_displayRefreshHz = 0.0f;
    std::optional<TextureSelfTestResult> m_textureSelfTest;

    std::optional<Clock::time_point> m_runStart;
    std::optional<Clock::time_point> m_lastFrameStart;
    Clock::time_point m_frameStart;
    double m_now = 0.0;
    bool m_measuring = false;
    bool m_finished = false;
    double m_measureStart = 0.0;
    double m_measureEnd = 0.0;
    double m_nextSample = 0.0;
    std::uint64_t m_frames = 0;
    std::uint64_t m_framesMeasured = 0;
    /// Measured frame intervals over 16.7 ms: a 60 FPS render loop meets the p99 criterion with at most 1% of its
    /// 60 frames per second over it, whatever rate this loop runs at.
    std::uint64_t m_longFrames = 0;
    /// Render frames of the window that uploaded at least one frame, and two or more.
    std::uint64_t m_framesWithUploads = 0;
    std::uint64_t m_framesWithSeveralUploads = 0;
    /// The current render frame's uploads: their CPU time and count.
    double m_frameUploadMs = 0.0;
    int m_frameUploads = 0;

    media::SampleRecorder m_frameIntervals;
    media::SampleRecorder m_frameWork;
    media::SampleRecorder m_uploadCpu;
    media::SampleRecorder m_uploadCopy;
    media::SampleRecorder m_uploadGpu;
    /// CPU time of every upload of a render frame together, over the render frames that uploaded.
    media::SampleRecorder m_uploadPerFrame;
    media::SampleRecorder m_machineBusy;
    std::vector<MemorySample> m_memory;
    CpuTimes m_cpuAtStart;
    CpuTimes m_cpuAtEnd;
    CpuTimes m_cpuLastSample;
    MemorySample m_memoryAtInit;

    bool m_screenshotWritten = false;
    std::optional<std::string> m_screenshotError;
};

} // namespace rv::spike
