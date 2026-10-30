#include <rv/xr/session.hpp>

#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace rv::xr {

std::string_view toString(SessionState state) noexcept {
    switch (state) {
    case SessionState::Unknown:
        return "unknown";
    case SessionState::Idle:
        return "idle";
    case SessionState::Ready:
        return "ready";
    case SessionState::Synchronized:
        return "synchronized";
    case SessionState::Visible:
        return "visible";
    case SessionState::Focused:
        return "focused";
    case SessionState::Stopping:
        return "stopping";
    case SessionState::LossPending:
        return "loss-pending";
    case SessionState::Exiting:
        return "exiting";
    }
    return "unknown";
}

Runtime::~Runtime() = default;

Session::Session(std::unique_ptr<Runtime> runtime) : m_runtime(std::move(runtime)) {}

Session::~Session() = default;

const RuntimeDescription& Session::description() const noexcept {
    return m_runtime->description();
}

void Session::record(std::string what) {
    m_events.push_back(SessionEvent{m_framesWaited, std::move(what)});
}

void Session::end(std::string reason) {
    if (m_ended) {
        return;
    }
    m_running = false;
    m_ended = true;
    m_frame.reset();
    m_endReason = std::move(reason);
    spdlog::info("XR session ended: {}", m_endReason);
}

Error Session::fail(const Error& error) {
    record("failed: " + error.toString());
    end(error.toString());
    return error;
}

void Session::pollEvents() {
    while (!m_ended) {
        const std::optional<RuntimeEvent> event = m_runtime->pollEvent();
        if (!event) {
            return;
        }
        switch (event->kind) {
        case RuntimeEvent::Kind::StateChanged:
            m_state = event->state;
            record(std::string(toString(m_state)));
            spdlog::info("XR session state: {}", toString(m_state));
            switch (m_state) {
            case SessionState::Ready:
                if (auto begun = m_runtime->beginSession(); begun) {
                    m_running = true;
                } else {
                    (void)fail(begun.error());
                }
                break;
            case SessionState::Focused:
                m_reachedFocused = true;
                break;
            case SessionState::Stopping:
                m_running = false;
                m_frame.reset();
                if (auto ended = m_runtime->endSession(); !ended) {
                    (void)fail(ended.error());
                }
                break;
            case SessionState::Exiting:
                end("exited");
                break;
            case SessionState::LossPending:
                end("lost");
                break;
            default:
                break;
            }
            break;
        case RuntimeEvent::Kind::InstanceLossPending:
            record("instance-loss-pending");
            end("instance lost");
            break;
        case RuntimeEvent::Kind::ReferenceSpaceChangePending:
            record("reference-space-change");
            m_spaceChanged = true;
            break;
        case RuntimeEvent::Kind::RefreshRateChanged:
            record(fmt::format("refresh-rate {:g} -> {:g} Hz", event->fromHz, event->toHz));
            spdlog::info("XR refresh rate: {:g} Hz -> {:g} Hz", event->fromHz, event->toHz);
            break;
        case RuntimeEvent::Kind::EventsLost:
            record(fmt::format("events-lost {}", event->count));
            break;
        }
    }
}

Expected<std::optional<Frame>> Session::beginFrame() {
    if (!m_running) {
        return std::optional<Frame>{};
    }
    auto timing = m_runtime->waitFrame();
    if (!timing) {
        return tl::unexpected(fail(timing.error()));
    }
    m_waited = std::chrono::steady_clock::now();
    ++m_framesWaited;
    if (auto begun = m_runtime->beginFrame(); !begun) {
        return tl::unexpected(fail(begun.error()));
    }
    const RuntimeDescription& description = m_runtime->description();
    Frame frame;
    frame.timing = *timing;
    frame.eyeSize = description.eyeSize;
    frame.srgb = description.srgb;
    frame.floorOffset = description.floorOffset;
    auto located = m_runtime->locateViews(timing->displayTimeNs, frame.eyes);
    if (!located) {
        return tl::unexpected(fail(located.error()));
    }
    frame.located = *located;
    // The recentering reaches the first frame located after it (a lost frame keeps it for the next).
    if (m_spaceChanged && frame.located) {
        frame.spaceChanged = true;
        m_spaceChanged = false;
    }
    if (frame.timing.shouldRender && frame.located) {
        for (int eye = 0; eye < 2; ++eye) {
            auto index = m_runtime->acquireImage(eye);
            if (!index) {
                return tl::unexpected(fail(index.error()));
            }
            const std::span<const std::uint32_t> images = m_runtime->images(eye);
            frame.images[static_cast<std::size_t>(eye)] = *index < images.size() ? images[*index] : 0;
        }
    }
    m_frame = frame;
    return m_frame;
}

Status Session::endFrame(bool rendered) {
    if (!m_frame) {
        return ok();
    }
    const Frame frame = *m_frame;
    m_frame.reset();
    if (frame.images[0] != 0 || frame.images[1] != 0) {
        for (int eye = 0; eye < 2; ++eye) {
            if (auto released = m_runtime->releaseImage(eye); !released) {
                return tl::unexpected(fail(released.error()));
            }
        }
    }
    const bool withViews = rendered && frame.wantsViews();
    if (auto ended = m_runtime->endFrame(frame.timing, withViews); !ended) {
        return tl::unexpected(fail(ended.error()));
    }
    const double cpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_waited).count();
    if (m_frames.size() >= kMaxFrameRecords) {
        return ok();
    }
    m_frames.push_back(
        FrameRecord{frame.timing.displayTimeNs, frame.timing.displayPeriodNs, withViews, cpuMs, std::nullopt});
    return ok();
}

void Session::requestExit() {
    if (m_running) {
        record("exit-requested");
        m_runtime->requestExit();
    }
}

FrameSummary Session::frameSummary() const {
    return summarizeFrames(m_frames);
}

std::optional<float> Session::currentRefreshRate() const {
    return m_runtime->currentRefreshRate();
}

} // namespace rv::xr
