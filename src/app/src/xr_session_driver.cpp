#include <rv/app/xr_session_driver.hpp>

#include <chrono>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/input/input_script.hpp>
#include <rv/xr/fake_runtime.hpp>
#include <rv/xr/gl_images.hpp>
#if defined(RV_XR_OPENXR)
#include <rv/xr/openxr_runtime.hpp>
#endif

namespace rv::app {
namespace {

/// How long an interactive run waits for a headset the runtime does not see yet; frame-limited runs never wait.
constexpr std::chrono::milliseconds kInteractiveHeadsetWait{15000};

/// The fake runtime's head at an input script's head pose.
xr::Pose headPoseOf(const input::ScriptedHeadPose& scripted, float standingHeight) {
    const xr::Angles angles{core::degreesToRadians(scripted.yawDeg), core::degreesToRadians(scripted.pitchDeg),
                            core::degreesToRadians(scripted.rollDeg)};
    return xr::Pose{xr::orientationOf(angles), scripted.position.value_or(glm::vec3(0.0f, standingHeight, 0.0f))};
}

void describe(const xr::RuntimeDescription& description, XrRecord& record) {
    record.runtime = description.runtime;
    record.runtimeVersion = description.runtimeVersion;
    record.system = description.system;
    record.extensions = description.extensions;
    record.space = description.space;
    record.recommendedEyeSize = description.recommendedEyeSize;
    record.eyeSize = description.eyeSize;
    record.renderScale = description.renderScale;
    record.format = description.format;
    record.srgb = description.srgb;
    record.imagesPerEye = description.imagesPerEye;
    record.refreshRates = description.refreshRates;
}

} // namespace

std::unique_ptr<XrSessionDriver> XrSessionDriver::start(const CommandLineOptions& options, XrRecord& record) {
    const XrOptions& xrOptions = options.xr;
    record.runtimeChoice = std::string(toString(xrOptions.runtime));
    const auto unavailable = [&](std::string why) -> std::unique_ptr<XrSessionDriver> {
        spdlog::warn("VR unavailable, the run stays on the desktop: {}", why);
        record.unavailable = std::move(why);
        return nullptr;
    };

    std::unique_ptr<xr::Runtime> runtime;
    const xr::FakeRuntime* fake = nullptr;
    if (xrOptions.runtime == XrRuntimeChoice::Fake) {
        xr::FakeRuntimeConfig config;
        config.renderScale = xrOptions.renderScale;
        config.loseAfterFrames = xrOptions.fakeLoseAfterFrames;
        if (options.inputScript) {
            auto loaded = input::InputScript::loadFromFile(*options.inputScript);
            if (!loaded) {
                return unavailable("the input script's head poses: " + loaded.error().toString());
            }
            config.headPose = [script = std::move(*loaded),
                               height = config.eyeHeight](std::uint64_t frame) -> std::optional<xr::Pose> {
                if (const std::optional<input::ScriptedHeadPose> pose = script.headPoseAt(frame)) {
                    return headPoseOf(*pose, height);
                }
                return std::nullopt;
            };
        }
        auto created = std::make_unique<xr::FakeRuntime>(xr::withGlImages(std::move(config)));
        if (created->images(0).empty() || created->images(1).empty()) {
            return unavailable("the fake runtime's swapchain images could not be created");
        }
        fake = created.get();
        runtime = std::move(created);
    } else {
#if defined(RV_XR_OPENXR)
        xr::OpenXrOptions openXr;
        openXr.runtimeJson = xrOptions.runtimeJson.value_or(std::filesystem::path());
        openXr.headsetWait = options.frameLimit ? std::chrono::milliseconds(0) : kInteractiveHeadsetWait;
        openXr.renderScale = xrOptions.renderScale;
        auto created = xr::createOpenXrRuntime(openXr);
        if (!created) {
            return unavailable(created.error().toString());
        }
        runtime = std::move(*created);
#else
        return unavailable("this build has no OpenXR loader (RV_ENABLE_XR off)");
#endif
    }

    describe(runtime->description(), record);
    record.started = true;
    const xr::RuntimeDescription& description = runtime->description();
    spdlog::info("VR: {} ({} {}), headset '{}', eyes {}x{} at render scale {:g}, {} space", description.backend,
                 description.runtime, description.runtimeVersion, description.system, description.eyeSize.x,
                 description.eyeSize.y, description.renderScale, description.space);
    return std::unique_ptr<XrSessionDriver>(
        new XrSessionDriver(std::make_unique<xr::Session>(std::move(runtime)), fake));
}

XrSessionDriver::XrSessionDriver(std::unique_ptr<xr::Session> session, const xr::FakeRuntime* fake)
    : m_session(std::move(session)), m_fake(fake) {}

XrSessionDriver::~XrSessionDriver() = default;

void XrSessionDriver::pollEvents() {
    m_session->pollEvents();
}

std::optional<xr::Frame> XrSessionDriver::beginFrame() {
    auto frame = m_session->beginFrame();
    if (!frame) {
        spdlog::error("VR frame failed: {}", frame.error().toString());
        return std::nullopt;
    }
    return *frame;
}

void XrSessionDriver::endFrame(bool rendered) {
    if (auto ended = m_session->endFrame(rendered); !ended) {
        spdlog::error("VR frame not submitted: {}", ended.error().toString());
    }
}

void XrSessionDriver::requestExit() {
    m_session->requestExit();
}

void XrSessionDriver::finish(XrRecord& record) const {
    describe(m_session->description(), record);
    record.refreshRate = m_session->currentRefreshRate();
    record.reachedFocused = m_session->reachedFocused();
    record.endReason = m_session->endReason();
    record.framesWaited = m_session->framesWaited();
    const xr::FrameSummary summary = m_session->frameSummary();
    record.framesWithViews = summary.rendered;
    record.refreshHz = summary.refreshHz;
    record.missedRefreshes = summary.missedRefreshes;
    record.missedFraction = summary.missedFraction;
    if (summary.cpu.count > 0) {
        record.cpuTimes = TimeSummary{summary.cpu.count, summary.cpu.p50Ms, summary.cpu.p95Ms, summary.cpu.p99Ms,
                                      summary.cpu.maxMs};
    }
    record.events.clear();
    for (const xr::SessionEvent& event : m_session->events()) {
        record.events.push_back(fmt::format("{}: {}", event.frame, event.what));
    }
    if (m_fake != nullptr) {
        spdlog::info("VR fake runtime: {} frame(s) ended, {} with both eyes", m_fake->framesEnded(),
                     m_fake->framesWithViews());
    }
}

} // namespace rv::app
