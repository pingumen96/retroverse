#pragma once

#include <rv/app/screen_policy.hpp>
#include <rv/core/aabb.hpp>
#include <rv/scene/render_item.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rv::gl {
class MaterialRegistry;
} // namespace rv::gl

namespace rv::scene {
class Camera;
struct SceneDescription;
} // namespace rv::scene

namespace rv::app {

class SceneInstance;
struct VideoRecord;

/// The hero CRT of a scene: its object, the material its screen is drawn with (a picture instance of MAT_Screen_CRT,
/// whose base colour texture the service overrides) and its world bounds (the view test that pauses the decoder).
struct HeroScreen {
    std::string objectId;
    scene::MaterialHandle material;
    core::Aabb bounds;
};

/// The first object of `description` built from the `crt` prefab whose screen shows a picture (its "screen" role has a
/// scene texture, so that its material has a base colour texture to override), with its screen's material in
/// `materials` (registered by SceneInstance under "<object id>/screen...") and the bounds of its first instance;
/// nothing when the scene has none (the model viewer, scenes without a TV).
[[nodiscard]] std::optional<HeroScreen> findHeroScreen(const scene::SceneDescription& description,
                                                       const SceneInstance& instance,
                                                       const gl::MaterialRegistry& materials);

struct ScreenServiceOptions {
    /// --video-freeze-pts: every clip opens paused, seeks to this time and holds that frame (the frame is waited for
    /// before the frame that shows it goes on, at most kFreezeWait), for goldens.
    std::optional<double> freezePtsSeconds;
    /// --fixed-dt (lockstep runs): a still picture being loaded is waited for (at most kStillWait), so that scripted
    /// runs show the same picture on the same frame.
    bool lockstep = false;
};

/// The hero CRT's screen service (I3 step 11, ADR-0012 §7): one media::VideoDecoder at a time (a runtime assertion:
/// never two, their threads included, which the run report's maxDecoderThreads shows), driven by a ScreenPolicy (what
/// to play: the dwell, the start choice, the fallback chain; the launch's life), its frames uploaded into one
/// gl::VideoTexture (kept for the run, the CPU section videoUpload around each upload), the still pictures of the chain
/// (screenshot, title screen, cover) decoded and fitted to the screen's 4:3 on a worker thread and uploaded as hero
/// textures, and whichever is shown set as the texture override of the screen's material (gl::MaterialRegistry::
/// setTextureOverride; the idle pattern is the material's own colour bars). The decoder pauses while the screen is out
/// of the view; before a launch it is stopped and its thread waited for, at most kScreenStopBoundSeconds (a thread that
/// does not end in time is logged and the launch goes on), and after the return it opens again at the clip's position;
/// once Retroverse quits it never starts again, and shutdown() waits for every decoder thread
/// (media::waitForStoppedDecoders()). A clip that replaces another opens once the other's thread ended (the screen
/// shows the new game's still meanwhile), so that no two decoder threads ever run. Files are logged by game, never by
/// path (the user's folders). Main thread only; the material registry must outlive the service.
class ScreenService {
public:
    /// Rows the video is decoded at, at most (ADR-0012 §7: one 480p decoder).
    static constexpr int kMaxVideoRows = 480;
    /// The still pictures' size: the screen's 4:3, as the clips.
    static constexpr glm::ivec2 kStillSize{640, 480};

    ScreenService(HeroScreen screen, gl::MaterialRegistry& materials, std::vector<ScreenGame> games,
                  ScreenServiceOptions options);
    ~ScreenService();
    ScreenService(const ScreenService&) = delete;
    ScreenService& operator=(const ScreenService&) = delete;

    /// The games changed (ScreenPolicy::setGames).
    void setGames(std::vector<ScreenGame> games);

    /// One frame at `nowSeconds` (the run's simulated time) seen by `camera`: the item focused or inspected
    /// (`hovered`, its catalog id), the decoder opened, paused, followed and its frames uploaded, the stills loaded,
    /// the screen's texture override set to what is shown.
    void update(double nowSeconds, const scene::Camera& camera, std::optional<std::string_view> hovered);

    /// A launch starts (before the window's hand-over): the decoder stops and its thread is waited for (bounded).
    void launchStarting();
    /// The launch ended, failed to start or was refused: the decoder opens again at its position.
    void launchEnded();
    /// The end of the run: the decoder stays stopped, every decoder thread is waited for, the worker ends.
    void shutdown();

    /// The run report's video section.
    void record(VideoRecord& record) const;
    /// Lines for the developer overlay's Stats panel.
    [[nodiscard]] std::vector<std::string> statusLines() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
