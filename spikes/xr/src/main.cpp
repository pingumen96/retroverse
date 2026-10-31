// spike_xr: I10 step 0, an OpenXR session on Retroverse's OpenGL context (docs/spikes/i10-xr.md, ADR-0017).
//
// Opens the active OpenXR runtime (or the one --runtime-json names), finds the headset, creates a session on the GL 4.5
// context of an SDL window through XR_KHR_opengl_enable, and renders a ray-cast room into one swapchain per eye at the
// runtime's pace (xrWaitFrame), showing the left eye in the window. The JSON report holds what the runtime offers
// (extensions, swapchain formats, reference spaces, refresh rates, the views' sizes and fields), the session's states,
// and the frames: the display period, the refreshes missed, the CPU and GPU time of each frame, the head's poses.
//
// Exit codes: 0 the run met the criteria (frames rendered, at most --max-missed of the refreshes missed), 1 it did
// not, 2 bad options or initialization failure (no runtime, no headset, no OpenGL support: the report says which), 3
// runtime failure.

#include <SDL3/SDL_main.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/main_thread.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/sdl_platform.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/gpu_timer.hpp>
#include <spike_xr/frame_stats.hpp>
#include <spike_xr/xr_math.hpp>

#include "room_scene.hpp"
#include "xr_session.hpp"

// Hybrid-GPU laptops: run on the discrete GPU, like retroverse.exe.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

namespace platform = rv::platform;
namespace spike = rv::spikes::xr;
using Clock = std::chrono::steady_clock;

constexpr int kExitOk = 0;
constexpr int kExitCriteriaMissed = 1;
constexpr int kExitInitialization = 2;
constexpr int kExitRuntime = 3;

constexpr float kNearPlane = 0.05f;
constexpr float kFarPlane = 100.0f;
/// How long the session gets to stop after the spike asks it to.
constexpr auto kExitGrace = std::chrono::seconds(3);
/// A head pose goes to the report this often.
constexpr auto kPoseSampleInterval = std::chrono::milliseconds(500);

struct SpikeOptions {
    std::filesystem::path dataDir;
    std::filesystem::path report;
    spike::InstanceOptions instance;
    spike::SessionOptions session;
    bool listOnly = false;
    std::chrono::milliseconds waitSystem{15000};
    int maxFrames = 0;
    double seconds = 30.0;
    double warmUpSeconds = 2.0;
    double maxMissed = 0.01;
    bool mirror = true;
    bool hidden = false;
    std::string sourceRevision;
};

double secondsBetween(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}

double millisecondsBetween(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

double rounded(double value, double unit = 1000.0) {
    return std::round(value * unit) / unit;
}

nlohmann::json describePose(const spike::EyePose& pose) {
    return {{"position", {rounded(pose.position.x), rounded(pose.position.y), rounded(pose.position.z)}},
            {"orientation",
             {rounded(pose.orientation.x), rounded(pose.orientation.y), rounded(pose.orientation.z),
              rounded(pose.orientation.w)}}};
}

nlohmann::json describeFov(const spike::EyeFov& fov) {
    const auto degrees = [](float radians) { return rounded(glm::degrees(static_cast<double>(radians)), 100.0); };
    return {{"leftDeg", degrees(fov.left)},
            {"rightDeg", degrees(fov.right)},
            {"upDeg", degrees(fov.up)},
            {"downDeg", degrees(fov.down)}};
}

class XrSpike {
public:
    explicit XrSpike(SpikeOptions options) : m_options(std::move(options)) {}

    int run() {
        const int code = runSteps();
        writeReport(code);
        return code;
    }

private:
    int runSteps() {
        if (auto initialized = initialize(); !initialized) {
            return fail(kExitInitialization, initialized.error().toString());
        }

        auto session = spike::OpenXrSession::createInstance(m_options.instance, m_report);
        if (!session) {
            return fail(kExitInitialization, session.error().toString());
        }
        m_xr = std::move(*session);
        if (auto found = m_xr->findSystem(m_options.listOnly ? std::chrono::milliseconds(0) : m_options.waitSystem,
                                          m_report);
            !found) {
            // Listing what the runtime offers needs no headset.
            return m_options.listOnly ? succeed("listed (no headset)") : fail(kExitInitialization, found.error().toString());
        }
        if (m_options.listOnly) {
            return succeed("listed");
        }

        if (auto created = m_xr->createSession(m_options.session, m_report); !created) {
            return fail(kExitInitialization, created.error().toString());
        }
        for (int eye = 0; eye < 2; ++eye) {
            auto framebuffers = spike::ImageFramebuffers::create(m_xr->images(eye), eye == 0 ? "xr.left" : "xr.right");
            if (!framebuffers) {
                return fail(kExitInitialization, framebuffers.error().toString());
            }
            m_eyeFramebuffers[eye] = std::move(*framebuffers);
        }

        if (auto looped = loop(); !looped) {
            m_xr->requestExit();
            return fail(kExitRuntime, looped.error().toString());
        }
        return summarize();
    }

    rv::Status initialize() {
        rv::core::markMainThread();
        std::error_code ec;
        std::filesystem::create_directories(m_options.dataDir / "logs", ec);
        rv::core::LogConfig logConfig;
        logConfig.directory = m_options.dataDir / "logs";
        logConfig.fileName = "spike-xr.log";
        rv::core::initLogging(logConfig);
        spdlog::info("{}: XR spike", rv::core::versionBanner());

        m_report["spike"] = "I10 step 0: an OpenXR session on the OpenGL context, a stereo room in the headset";
        m_report["build"] = {{"version", std::string(rv::core::kVersion)},
                             {"sourceRevision", m_options.sourceRevision},
#ifdef NDEBUG
                             {"config", "release"}
#else
                             {"config", "debug"}
#endif
        };
        m_report["options"] = {{"listOnly", m_options.listOnly},
                               {"runtimeJson", rv::core::pathToUtf8(m_options.instance.runtimeJson)},
                               {"renderScale", m_options.session.renderScale},
                               {"space", spike::toString(m_options.session.space)},
                               {"seconds", m_options.seconds},
                               {"frames", m_options.maxFrames},
                               {"warmUpSeconds", m_options.warmUpSeconds},
                               {"maxMissed", m_options.maxMissed},
                               {"mirror", m_options.mirror}};
        if (m_options.session.refreshRate) {
            m_report["options"]["refreshRate"] = *m_options.session.refreshRate;
        }

        auto sdl = platform::SdlPlatform::init(
            {"Retroverse XR spike", std::string(rv::core::kVersion), "io.github.pingumen96.retroverse.spikexr"});
        if (!sdl) {
            return rv::propagate(sdl.error());
        }
        m_sdl.emplace(std::move(*sdl));

        platform::WindowDesc windowDesc;
        windowDesc.title = "Retroverse XR spike (left eye)";
        windowDesc.width = 960;
        windowDesc.height = 540;
        windowDesc.hidden = m_options.hidden;
        platform::GlContextDesc contextDesc;
        // The runtime paces the frames (xrWaitFrame); the mirror's swap must never wait for the monitor.
        contextDesc.vsync = false;
        auto window = platform::createGraphicsWindow(windowDesc, contextDesc);
        if (!window) {
            return rv::propagate(window.error());
        }
        m_window.emplace(std::move(*window));
        auto device = rv::gl::GLDevice::create(m_window->glLoader(), rv::gl::GLDeviceConfig{});
        if (!device) {
            return rv::propagate(device.error());
        }
        m_device = std::move(*device);
        const rv::gl::GLCapabilities& caps = m_device->caps();
        m_report["gl"] = {{"vendor", caps.vendor}, {"renderer", caps.renderer}, {"version", caps.version}};

        auto scene = spike::RoomScene::create();
        if (!scene) {
            return rv::propagate(scene.error());
        }
        m_scene.emplace(std::move(*scene));
        auto timer = rv::gl::GpuTimer::create("spike_xr.eyes");
        if (!timer) {
            return rv::propagate(timer.error());
        }
        m_gpuTimer = std::move(*timer);
        m_start = Clock::now();
        return {};
    }

    /// The frame loop, until the frame or time limit, the window closing, Esc, or the runtime ending the session.
    rv::Status loop() {
        bool exitRequested = false;
        std::optional<Clock::time_point> exitRequestedAt;
        std::optional<Clock::time_point> firstFrame;
        Clock::time_point lastPoseSample{};
        Clock::time_point lastCounterSample{};
        m_frames.reserve(1u << 15);

        for (;;) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                const bool escape = event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE;
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || escape) {
                    m_stopReason = m_stopReason.empty() ? "window closed" : m_stopReason;
                    exitRequested = true;
                }
            }

            m_xr->pollEvents();
            if (m_xr->ended()) {
                if (m_stopReason.empty()) {
                    m_stopReason = "the runtime ended the session";
                }
                return {};
            }
            const Clock::time_point now = Clock::now();
            if (firstFrame) {
                const bool timeUp = m_options.seconds > 0.0 && secondsBetween(*firstFrame, now) >= m_options.seconds;
                const bool framesUp = m_options.maxFrames > 0 && static_cast<int>(m_frames.size()) >= m_options.maxFrames;
                if ((timeUp || framesUp) && m_stopReason.empty()) {
                    m_stopReason = timeUp ? "time limit" : "frame limit";
                    exitRequested = true;
                }
            }
            if (exitRequested && !exitRequestedAt) {
                exitRequestedAt = now;
                m_xr->requestExit();
            }
            if (exitRequestedAt && (now - *exitRequestedAt > kExitGrace || !m_xr->running())) {
                return {};
            }
            if (!m_xr->running()) {
                // Not started yet (waiting for READY), or stopped: nothing to render.
                SDL_WaitEventTimeout(nullptr, 10);
                continue;
            }

            auto timing = m_xr->waitFrame();
            if (!timing) {
                return rv::propagate(timing.error());
            }
            const Clock::time_point frameStart = Clock::now();
            if (auto begun = m_xr->beginFrame(); !begun) {
                return begun;
            }

            bool rendered = false;
            std::array<spike::EyeView, 2> eyes{};
            if (timing->shouldRender) {
                auto located = m_xr->locateViews(timing->displayTimeNs, eyes);
                if (!located) {
                    return rv::propagate(located.error());
                }
                if (*located) {
                    if (auto drawn = renderEyes(eyes, secondsBetween(m_start, frameStart)); !drawn) {
                        return drawn;
                    }
                    rendered = true;
                } else {
                    ++m_trackingLost;
                }
            }
            if (auto ended = m_xr->endFrame(*timing, rendered); !ended) {
                return ended;
            }
            // The runtime may use the context in xrEndFrame: forget the cached state.
            m_device->state().invalidate();
            if (m_options.mirror && !m_options.hidden && rendered) {
                (void)m_window->swap();
            }

            const Clock::time_point frameEnd = Clock::now();
            if (rendered && !firstFrame) {
                firstFrame = frameEnd;
                m_report["firstFrameMs"] = rounded(millisecondsBetween(m_start, frameEnd), 10.0);
            }
            spike::FrameRecord record{timing->displayTimeNs, timing->displayPeriodNs, rendered,
                                      millisecondsBetween(frameStart, frameEnd), m_gpuTimer.latestMilliseconds()};
            const bool warmingUp = !firstFrame || secondsBetween(*firstFrame, frameEnd) < m_options.warmUpSeconds;
            (warmingUp ? m_warmUpFrames : m_frames).push_back(record);
            if (!warmingUp && frameEnd - lastCounterSample >= kPoseSampleInterval) {
                lastCounterSample = frameEnd;
                sampleCounters();
            }
            if (rendered) {
                sampleHead(eyes, frameEnd, lastPoseSample);
            }
        }
    }

    rv::Status renderEyes(const std::array<spike::EyeView, 2>& eyes, double timeSeconds) {
        const glm::ivec2 size = m_xr->eyeSize();
        m_gpuTimer.begin();
        for (int eye = 0; eye < 2; ++eye) {
            auto image = m_xr->acquireImage(eye);
            if (!image) {
                m_gpuTimer.end();
                return rv::propagate(image.error());
            }
            const std::uint32_t framebuffer = m_eyeFramebuffers[eye].framebuffer(*image);
            const glm::mat4 viewProjection = spike::projectionFromFov(eyes[eye].fov, kNearPlane, kFarPlane) *
                                             spike::viewFromPose(eyes[eye].pose);
            m_scene->render(*m_device, framebuffer, size, viewProjection, m_xr->floorOffset(), m_xr->srgb(),
                            timeSeconds);
            if (eye == 0 && m_options.mirror && !m_options.hidden) {
                spike::blitToWindow(*m_device, framebuffer, size, m_window->drawableSize());
            }
            if (auto released = m_xr->releaseImage(eye); !released) {
                m_gpuTimer.end();
                return released;
            }
        }
        m_gpuTimer.end();
        return {};
    }

    void sampleHead(const std::array<spike::EyeView, 2>& eyes, Clock::time_point now, Clock::time_point& last) {
        const glm::vec3 head = (eyes[0].pose.position + eyes[1].pose.position) * 0.5f;
        if (!m_headMin) {
            m_headMin = head;
            m_headMax = head;
            m_report["eyesAtStart"] = {{"left", {{"pose", describePose(eyes[0].pose)}, {"fov", describeFov(eyes[0].fov)}}},
                                       {"right", {{"pose", describePose(eyes[1].pose)}, {"fov", describeFov(eyes[1].fov)}}}};
        }
        m_headMin = glm::min(*m_headMin, head);
        m_headMax = glm::max(*m_headMax, head);
        m_eyeDistances.push_back(glm::distance(eyes[0].pose.position, eyes[1].pose.position));
        if (now - last >= kPoseSampleInterval) {
            last = now;
            nlohmann::json sample = describePose(eyes[0].pose);
            sample["atMs"] = rounded(millisecondsBetween(m_start, now), 10.0);
            m_poses.push_back(std::move(sample));
        }
    }

    void sampleCounters() {
        for (const spike::RuntimeCounter& counter : m_xr->queryCounters()) {
            CounterSeries& series = m_counters[counter.path];
            series.unit = counter.unit;
            series.values.push_back(counter.value);
        }
    }

    /// Each runtime counter over the measured frames: whether it counts up or reads a level, the samples show.
    nlohmann::json describeCounters() const {
        nlohmann::json counters = nlohmann::json::object();
        for (const auto& [path, series] : m_counters) {
            const auto [low, high] = std::minmax_element(series.values.begin(), series.values.end());
            double sum = 0.0;
            for (const double value : series.values) {
                sum += value;
            }
            counters[path] = {{"unit", series.unit},
                              {"samples", series.values.size()},
                              {"first", rounded(series.values.front())},
                              {"last", rounded(series.values.back())},
                              {"min", rounded(*low)},
                              {"max", rounded(*high)},
                              {"mean", rounded(sum / static_cast<double>(series.values.size()))}};
        }
        return counters;
    }

    int summarize() {
        const spike::FrameSummary summary = spike::summarizeFrames(m_frames);
        m_report["frames"] = spike::toJson(summary);
        m_report["warmUp"] = spike::toJson(spike::summarizeFrames(m_warmUpFrames));
        m_report["trackingLostFrames"] = m_trackingLost;
        m_report["reachedFocused"] = m_xr->reachedFocused();
        if (const auto rate = m_xr->currentRefreshRate()) {
            m_report["refreshRateAtEnd"] = *rate;
        }
        if (!m_eyeDistances.empty()) {
            std::vector<float> sorted = m_eyeDistances;
            std::sort(sorted.begin(), sorted.end());
            m_report["eyeDistanceMm"] = rounded(sorted[sorted.size() / 2] * 1000.0, 10.0);
        }
        if (m_headMin) {
            const glm::vec3 range = *m_headMax - *m_headMin;
            m_report["headRangeM"] = {rounded(range.x), rounded(range.y), rounded(range.z)};
        }
        m_report["poses"] = m_poses;
        m_report["runtimeCounters"] = describeCounters();
        spdlog::info("{} frames at {:.1f} Hz, {} refreshes missed ({:.2f}%), CPU p99 {:.2f} ms, GPU p99 {:.2f} ms",
                     summary.frames, summary.refreshHz, summary.missedRefreshes, summary.missedFraction * 100.0,
                     summary.cpu.p99Ms, summary.gpu.p99Ms);
        if (!spike::meetsCriteria(summary, m_options.maxMissed)) {
            return fail(kExitCriteriaMissed,
                        summary.rendered == 0
                            ? "no frame was rendered (the session never became visible?)"
                            : fmt::format("{:.2f}% of the refreshes missed (limit {:.2f}%)",
                                          summary.missedFraction * 100.0, m_options.maxMissed * 100.0));
        }
        return succeed(m_stopReason);
    }

    int fail(int code, const std::string& reason) {
        spdlog::error("{}", reason);
        m_report["result"] = {{"ok", false}, {"reason", reason}};
        return code;
    }

    int succeed(const std::string& reason) {
        m_report["result"] = {{"ok", true}, {"reason", reason}};
        return kExitOk;
    }

    void writeReport(int code) {
        m_report["exitCode"] = code;
        if (m_xr) {
            m_report["events"] = m_xr->events();
        }
        if (auto written = rv::core::writeTextFileAtomic(m_options.report, m_report.dump(2)); !written) {
            spdlog::error("The report could not be written: {}", written.error().toString());
            return;
        }
        spdlog::info("Report: {}", rv::core::pathToUtf8(m_options.report));
    }

    /// A runtime counter's samples over the measured frames, one every kPoseSampleInterval.
    struct CounterSeries {
        std::string unit;
        std::vector<double> values;
    };

    SpikeOptions m_options;
    nlohmann::json m_report = nlohmann::json::object();
    // Destroyed in reverse: the session's swapchains go before the GL context they live in.
    std::optional<platform::SdlPlatform> m_sdl;
    std::optional<platform::GraphicsWindow> m_window;
    std::unique_ptr<rv::gl::GLDevice> m_device;
    std::optional<spike::RoomScene> m_scene;
    rv::gl::GpuTimer m_gpuTimer;
    std::array<spike::ImageFramebuffers, 2> m_eyeFramebuffers;
    std::unique_ptr<spike::OpenXrSession> m_xr;
    Clock::time_point m_start;
    std::vector<spike::FrameRecord> m_frames;
    std::vector<spike::FrameRecord> m_warmUpFrames;
    std::vector<float> m_eyeDistances;
    std::optional<glm::vec3> m_headMin;
    std::optional<glm::vec3> m_headMax;
    nlohmann::json m_poses = nlohmann::json::array();
    std::map<std::string, CounterSeries> m_counters;
    std::size_t m_trackingLost = 0;
    std::string m_stopReason;
};

std::optional<spike::SpaceChoice> parseSpace(const std::string& text) {
    for (const auto space : {spike::SpaceChoice::Auto, spike::SpaceChoice::LocalFloor, spike::SpaceChoice::Stage,
                             spike::SpaceChoice::Local}) {
        if (text == spike::toString(space)) {
            return space;
        }
    }
    return std::nullopt;
}

std::optional<SpikeOptions> parseOptions(int argc, char* argv[], int& exitCode) {
    rv::core::ArgParser parser("spike_xr", "I10 step 0: an OpenXR session on the OpenGL context, a stereo room in the "
                                           "headset, frame pacing measured (docs/spikes/i10-xr.md)");
    parser.option("data-dir", "dir", "Logs (and the default report) go here")
        .option("report", "file", "The JSON report (default <data-dir>/xr-report.json)")
        .option("runtime-json", "file", "The manifest of the runtime to use instead of the active one")
        .flag("list", "Only list what the runtime and the headset offer (no session)")
        .option("wait-system-s", "seconds", "How long to wait for the headset to connect", "15")
        .option("seconds", "seconds", "Run this long after the first frame (0: until Esc or the session ends)", "30")
        .option("frames", "count", "Stop after this many measured frames (0: no limit)", "0")
        .option("warmup-s", "seconds", "Frames this soon after the first are reported apart", "2")
        .option("render-scale", "factor", "The eyes' size over the runtime's recommendation", "1.0")
        .option("refresh-rate", "hz", "Ask for this refresh rate (XR_FB_display_refresh_rate)")
        .option("space", "auto|local-floor|stage|local", "The reference space of the room", "auto")
        .option("max-missed", "fraction", "The largest fraction of missed refreshes that passes", "0.01")
        .flag("no-mirror", "Do not show the left eye in the window")
        .flag("hidden", "Never show the window (implies --no-mirror)")
        .option("source-revision", "text", "What the build was made from (recorded in the report)");

    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
    const auto parsed = parser.parse(arguments);
    if (!parsed) {
        std::fprintf(stderr, "%s\n\n%s", parsed.error().toString().c_str(), parser.helpText().c_str());
        exitCode = kExitInitialization;
        return std::nullopt;
    }
    if (parsed->has("help")) {
        std::fputs(parser.helpText().c_str(), stdout);
        exitCode = kExitOk;
        return std::nullopt;
    }

    SpikeOptions options;
    const auto bad = [&](const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
        exitCode = kExitInitialization;
        return std::nullopt;
    };
    const auto waitSystem = parsed->doubleValue("wait-system-s", 15.0);
    const auto seconds = parsed->doubleValue("seconds", 30.0);
    const auto frames = parsed->intValue("frames", 0);
    const auto warmUp = parsed->doubleValue("warmup-s", 2.0);
    const auto scale = parsed->doubleValue("render-scale", 1.0);
    const auto maxMissed = parsed->doubleValue("max-missed", 0.01);
    if (!waitSystem || !seconds || !frames || !warmUp || !scale || !maxMissed) {
        return bad("A numeric option is malformed");
    }
    if (*scale <= 0.0 || *scale > 4.0) {
        return bad("--render-scale must be in (0, 4]");
    }
    const auto space = parseSpace(parsed->value("space").value_or("auto"));
    if (!space) {
        return bad("--space must be auto, local-floor, stage or local");
    }
    if (const auto rate = parsed->value("refresh-rate")) {
        const auto value = parsed->doubleValue("refresh-rate", 0.0);
        if (!value || *value <= 0.0) {
            return bad("--refresh-rate must be a positive number");
        }
        options.session.refreshRate = static_cast<float>(*value);
    }
    options.waitSystem = std::chrono::milliseconds(static_cast<long long>(*waitSystem * 1000.0));
    options.seconds = *seconds;
    options.maxFrames = *frames;
    options.warmUpSeconds = *warmUp;
    options.session.renderScale = *scale;
    options.session.space = *space;
    options.maxMissed = *maxMissed;
    options.listOnly = parsed->has("list");
    options.hidden = parsed->has("hidden");
    options.mirror = !parsed->has("no-mirror") && !options.hidden;
    options.sourceRevision = parsed->value("source-revision").value_or("");

    const auto absolute = [](const std::filesystem::path& path) {
        std::error_code ec;
        const std::filesystem::path result = std::filesystem::absolute(path, ec);
        return ec ? path : result;
    };
    if (const auto runtime = parsed->value("runtime-json")) {
        options.instance.runtimeJson = absolute(rv::core::pathFromUtf8(*runtime));
    }
    options.dataDir = parsed->value("data-dir") ? absolute(rv::core::pathFromUtf8(*parsed->value("data-dir")))
                                                : rv::core::executablePath().parent_path() / "spike-xr-data";
    options.report = parsed->value("report") ? absolute(rv::core::pathFromUtf8(*parsed->value("report")))
                                             : options.dataDir / "xr-report.json";
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    int exitCode = kExitOk;
    std::optional<SpikeOptions> options = parseOptions(argc, argv, exitCode);
    if (!options) {
        return exitCode;
    }
    try {
        XrSpike spike(std::move(*options));
        exitCode = spike.run();
    } catch (const std::exception& e) {
        spdlog::critical("spike_xr failed: {}", e.what());
        exitCode = kExitRuntime;
    } catch (...) {
        spdlog::critical("spike_xr failed with an unknown exception");
        exitCode = kExitRuntime;
    }
    rv::core::shutdownLogging();
    return exitCode;
}
