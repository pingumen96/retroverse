#include <rv/xr/fake_runtime.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <rv/xr/swapchain_format.hpp>

namespace rv::xr {
namespace {

constexpr int kMinEyeSide = 16;
constexpr int kMaxEyeSide = 4096;

int scaledSide(int recommended, double scale) noexcept {
    const double side = std::round(static_cast<double>(recommended) * scale);
    return static_cast<int>(std::clamp(side, static_cast<double>(kMinEyeSide), static_cast<double>(kMaxEyeSide)));
}

Error notRunning(const char* call) {
    return Error{ErrorCode::InvalidArgument, std::string(call) + " without a running session", "fake runtime"};
}

} // namespace

FakeRuntime::FakeRuntime(FakeRuntimeConfig config) : m_config(std::move(config)) {
    m_description.backend = "fake";
    m_description.runtime = "Retroverse fake runtime";
    m_description.runtimeVersion = "1.0.0";
    m_description.system = "Fake headset";
    m_description.space = "local-floor";
    m_description.recommendedEyeSize = m_config.recommendedEyeSize;
    m_description.eyeSize = glm::ivec2(scaledSide(m_config.recommendedEyeSize.x, m_config.renderScale),
                                       scaledSide(m_config.recommendedEyeSize.y, m_config.renderScale));
    m_description.renderScale = m_config.renderScale;
    m_description.format = glFormatName(kGlSrgb8Alpha8);
    m_description.srgb = true;
    m_description.imagesPerEye = std::max<std::size_t>(1, m_config.imagesPerEye);
    m_description.refreshRates = m_config.refreshRates;
    if (m_description.refreshRates.empty() && m_config.displayPeriodNs > 0) {
        m_description.refreshRates = {static_cast<float>(1.0e9 / static_cast<double>(m_config.displayPeriodNs))};
    }
    if (!m_description.refreshRates.empty()) {
        m_refreshRate = m_description.refreshRates.front();
    }
    std::uint32_t next = 1;
    for (std::vector<std::uint32_t>& images : m_images) {
        if (m_config.createImages) {
            images = m_config.createImages(m_description.eyeSize, m_description.imagesPerEye);
        } else {
            for (std::size_t i = 0; i < m_description.imagesPerEye; ++i) {
                images.push_back(next++);
            }
        }
    }
    queueState(SessionState::Idle);
    queueState(SessionState::Ready);
    // I10 step 9: the rate asked for, as XR_FB_display_refresh_rate switches it.
    if (m_config.refreshRate) {
        m_description.refreshRateRequested = m_config.refreshRate;
        if (const std::optional<float> chosen = chooseRefreshRate(m_description.refreshRates, *m_config.refreshRate);
            chosen && m_refreshRate && *chosen != *m_refreshRate) {
            RuntimeEvent event;
            event.kind = RuntimeEvent::Kind::RefreshRateChanged;
            event.fromHz = *m_refreshRate;
            event.toHz = *chosen;
            m_events.push_back(event);
            m_refreshRate = chosen;
            m_config.displayPeriodNs = static_cast<std::int64_t>(std::llround(1.0e9 / static_cast<double>(*chosen)));
        }
    }
}

FakeRuntime::~FakeRuntime() {
    if (m_config.destroyImages) {
        for (const std::vector<std::uint32_t>& images : m_images) {
            m_config.destroyImages(images);
        }
    }
}

void FakeRuntime::queueState(SessionState state) {
    RuntimeEvent event;
    event.kind = RuntimeEvent::Kind::StateChanged;
    event.state = state;
    m_events.push_back(event);
}

std::optional<RuntimeEvent> FakeRuntime::pollEvent() {
    if (m_events.empty()) {
        return std::nullopt;
    }
    const RuntimeEvent event = m_events.front();
    m_events.pop_front();
    if (event.kind == RuntimeEvent::Kind::StateChanged) {
        m_state = event.state;
    }
    return event;
}

Status FakeRuntime::beginSession() {
    if (m_state != SessionState::Ready || m_begun) {
        return makeError(ErrorCode::InvalidArgument, "the session is not ready", "fake runtime");
    }
    m_begun = true;
    queueState(SessionState::Synchronized);
    queueState(SessionState::Visible);
    queueState(SessionState::Focused);
    return ok();
}

Status FakeRuntime::endSession() {
    if (!m_begun) {
        return tl::unexpected(notRunning("endSession"));
    }
    m_begun = false;
    queueState(SessionState::Idle);
    queueState(SessionState::Exiting);
    return ok();
}

void FakeRuntime::requestExit() {
    if (m_begun) {
        queueState(SessionState::Stopping);
    }
}

Expected<FrameTiming> FakeRuntime::waitFrame() {
    if (!m_begun) {
        return tl::unexpected(notRunning("waitFrame"));
    }
    ++m_framesWaited;
    m_displayTimeNs += m_config.displayPeriodNs;
    if (m_config.loseAfterFrames && m_framesWaited == *m_config.loseAfterFrames) {
        queueState(SessionState::LossPending);
    }
    if (std::find(m_config.recenterFrames.begin(), m_config.recenterFrames.end(), m_framesWaited - 1) !=
        m_config.recenterFrames.end()) {
        RuntimeEvent event;
        event.kind = RuntimeEvent::Kind::ReferenceSpaceChangePending;
        m_events.push_back(event);
    }
    const bool shown = m_state == SessionState::Visible || m_state == SessionState::Focused;
    return FrameTiming{shown, m_displayTimeNs, m_config.displayPeriodNs};
}

Status FakeRuntime::beginFrame() {
    if (!m_begun) {
        return tl::unexpected(notRunning("beginFrame"));
    }
    m_frameBegun = true;
    return ok();
}

Expected<bool> FakeRuntime::locateViews(std::int64_t /*displayTimeNs*/, EyeViews& eyes) {
    if (!m_begun) {
        return tl::unexpected(notRunning("locateViews"));
    }
    const std::uint64_t frame = m_framesWaited > 0 ? m_framesWaited - 1 : 0;
    if (std::find(m_config.untrackedFrames.begin(), m_config.untrackedFrames.end(), frame) !=
        m_config.untrackedFrames.end()) {
        return false;
    }
    Pose head{glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f, m_config.eyeHeight, 0.0f)};
    if (m_config.headPose) {
        if (const std::optional<Pose> scripted = m_config.headPose(frame)) {
            head = *scripted;
        }
    }
    head.orientation = glm::normalize(head.orientation);
    const glm::vec3 halfOffset = head.orientation * glm::vec3(0.5f * m_config.ipdMeters, 0.0f, 0.0f);
    eyes[0] = EyeView{Pose{head.orientation, head.position - halfOffset}, m_config.leftField};
    eyes[1] = EyeView{Pose{head.orientation, head.position + halfOffset}, scene::mirroredField(m_config.leftField)};
    return true;
}

Expected<std::uint32_t> FakeRuntime::acquireImage(int eye) {
    const auto index = static_cast<std::size_t>(eye);
    if (!m_frameBegun || index >= m_images.size() || m_acquired[index]) {
        return makeError(ErrorCode::InvalidArgument, "acquireImage out of order", "fake runtime");
    }
    m_acquired[index] = true;
    const std::uint32_t image = m_next[index];
    m_next[index] = static_cast<std::uint32_t>((image + 1) % m_images[index].size());
    return image;
}

Status FakeRuntime::releaseImage(int eye) {
    const auto index = static_cast<std::size_t>(eye);
    if (index >= m_images.size() || !m_acquired[index]) {
        return makeError(ErrorCode::InvalidArgument, "releaseImage without an acquired image", "fake runtime");
    }
    m_acquired[index] = false;
    return ok();
}

Status FakeRuntime::endFrame(const FrameTiming& /*timing*/, bool withViews) {
    if (!m_frameBegun || m_acquired[0] || m_acquired[1]) {
        return makeError(ErrorCode::InvalidArgument, "endFrame without a begun frame or with an image held",
                         "fake runtime");
    }
    m_frameBegun = false;
    ++m_framesEnded;
    m_framesWithViews += withViews ? 1 : 0;
    return ok();
}

std::span<const std::uint32_t> FakeRuntime::images(int eye) const {
    const auto index = static_cast<std::size_t>(eye);
    return index < m_images.size() ? std::span<const std::uint32_t>(m_images[index]) : std::span<const std::uint32_t>{};
}

Expected<ControllerState> FakeRuntime::syncControllers(std::int64_t /*displayTimeNs*/) {
    if (!m_begun) {
        return tl::unexpected(notRunning("syncControllers"));
    }
    ControllerState state;
    const std::uint64_t frame = m_framesWaited > 0 ? m_framesWaited - 1 : 0;
    // I10 step 10: the thumbsticks, as the pad's sticks on their side.
    if (m_config.handStick) {
        for (const Hand hand : {Hand::Left, Hand::Right}) {
            const glm::vec2 stick = m_config.handStick(frame, hand);
            const bool right = hand == Hand::Right;
            state.pad.axes[input::toIndex(right ? input::GamepadAxis::RightX : input::GamepadAxis::LeftX)] = stick.x;
            state.pad.axes[input::toIndex(right ? input::GamepadAxis::RightY : input::GamepadAxis::LeftY)] = stick.y;
        }
    }
    // I11 step 14: the left menu button, the pad's Start as xr_bindings.json maps it.
    if (m_config.leftMenu && m_config.leftMenu(frame)) {
        state.pad.buttons.set(input::toIndex(input::GamepadButton::Start));
    }
    if (!m_config.handPose) {
        return state;
    }
    for (const Hand hand : {Hand::Left, Hand::Right}) {
        if (const std::optional<Pose> pose = m_config.handPose(frame, hand)) {
            HandState& held = state.hands[toIndex(hand)];
            held.tracked = true;
            held.grip = *pose;
            held.aim = *pose;
            held.select = m_config.handSelect ? m_config.handSelect(frame, hand) : 0.0f;
        }
    }
    return state;
}

void FakeRuntime::vibrate(Hand hand, float /*amplitude*/, float /*seconds*/) {
    ++m_vibrations[toIndex(hand)];
}

std::optional<float> FakeRuntime::currentRefreshRate() const {
    return m_refreshRate;
}

} // namespace rv::xr
