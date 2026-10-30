#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <glm/vec2.hpp>

#include <rv/scene/camera.hpp>
#include <rv/scene/stereo.hpp>
#include <rv/xr/runtime.hpp>

namespace rv::xr {

/// What the fake runtime pretends to be.
struct FakeRuntimeConfig {
    /// The eyes' recommended size; the session renders at it times `renderScale`.
    glm::ivec2 recommendedEyeSize{1024, 1024};
    double renderScale = 1.0;
    /// The left eye's field (a Quest 3S's, like the stereo preview's); the right eye's mirrors it.
    scene::EyeFov leftField = scene::stereoPreviewLeftField();
    float ipdMeters = scene::kStereoPreviewIpdMeters;
    /// 72 Hz.
    std::int64_t displayPeriodNs = 13'888'889;
    /// The head's height above the space's origin (LOCAL_FLOOR: the floor) when no pose is given.
    float eyeHeight = 1.65f;
    std::size_t imagesPerEye = 3;
    /// Creates `count` images of `size` for one eye's swapchain and returns their names (GL textures in the app, any
    /// non-zero numbers in unit tests), and destroys them with the runtime. Without it the images are 1, 2, ...
    std::function<std::vector<std::uint32_t>(glm::ivec2 size, std::size_t count)> createImages;
    std::function<void(std::span<const std::uint32_t> images)> destroyImages;
    /// The head's pose in the space at the n-th frame waited for (0 first): the input script's head poses. Nothing,
    /// or no function: standing at the origin, `eyeHeight` up, facing -Z.
    std::function<std::optional<Pose>(std::uint64_t frame)> headPose;
    /// The session is lost (LossPending) when this many frames were waited for: the desktop fallback's tests.
    std::optional<std::uint64_t> loseAfterFrames;
    /// Tracking is lost on these frames (locateViews says so): frames with no layer.
    std::vector<std::uint64_t> untrackedFrames;
};

/// A runtime without a headset (ADR-0017 §3, §10): two asymmetric eyes `ipdMeters` apart at the scripted head pose, a
/// steady display period with no wait (frames go as fast as the app renders them, and with --fixed-dt reproducibly),
/// swapchains of images the app creates. Its session becomes ready at once, then synchronized, visible and focused
/// once begun; requestExit() stops it, and endSession() makes it exit. GPU-free.
class FakeRuntime final : public Runtime {
public:
    explicit FakeRuntime(FakeRuntimeConfig config);
    ~FakeRuntime() override;

    [[nodiscard]] const RuntimeDescription& description() const noexcept override { return m_description; }
    [[nodiscard]] std::optional<RuntimeEvent> pollEvent() override;
    [[nodiscard]] Status beginSession() override;
    [[nodiscard]] Status endSession() override;
    void requestExit() override;
    [[nodiscard]] Expected<FrameTiming> waitFrame() override;
    [[nodiscard]] Status beginFrame() override;
    [[nodiscard]] Expected<bool> locateViews(std::int64_t displayTimeNs, EyeViews& eyes) override;
    [[nodiscard]] Expected<std::uint32_t> acquireImage(int eye) override;
    [[nodiscard]] Status releaseImage(int eye) override;
    [[nodiscard]] Status endFrame(const FrameTiming& timing, bool withViews) override;
    [[nodiscard]] std::span<const std::uint32_t> images(int eye) const override;
    [[nodiscard]] std::optional<float> currentRefreshRate() const override;

    /// What the app submitted: frames ended, and those with both eyes.
    [[nodiscard]] std::uint64_t framesEnded() const noexcept { return m_framesEnded; }
    [[nodiscard]] std::uint64_t framesWithViews() const noexcept { return m_framesWithViews; }

private:
    void queueState(SessionState state);

    FakeRuntimeConfig m_config;
    RuntimeDescription m_description;
    std::deque<RuntimeEvent> m_events;
    SessionState m_state = SessionState::Unknown;
    bool m_begun = false;
    bool m_frameBegun = false;
    std::uint64_t m_framesWaited = 0;
    std::int64_t m_displayTimeNs = 0;
    std::array<std::vector<std::uint32_t>, 2> m_images;
    std::array<std::uint32_t, 2> m_next{};
    std::array<bool, 2> m_acquired{};
    std::uint64_t m_framesEnded = 0;
    std::uint64_t m_framesWithViews = 0;
};

} // namespace rv::xr
