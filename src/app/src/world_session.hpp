#pragma once

// The room in the app (I3 step 10b; ADR-0010 §12, ADR-0011 §1, §6, ADR-0012 §2, §3): what retroverse.exe shows without
// --scene, --model or --mock-catalog. While the room is made (app::WorldFlow: the stored plan read, the snapshot on the
// catalog worker, the plan and the scene on the worker pool) the empty room's shell shows with the "generating" notice
// (the WorldGenerating state); the result is committed at a frame boundary: the room's static part through the
// application's scene (SceneHost::showScene: a SceneInstance of its SceneDescription, the walker at its doorway
// camera), its collection on a static instance group (CollectionInstance) whose covers and spines the cover loader
// brings in within the upload budget, the pick index of its items (world::PickIndex) for the hover outline. A catalog
// that does not open or has no games of the room's system gives the test room (SceneHost::showTestRoom), a room that
// cannot be made now the shell, each with its notice. Lockstep runs (--fixed-dt) wait between frames, at most
// kLockstepWaitLimit at a time, while the room or its layers are on their way. Main thread only.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <rv/app/run_report.hpp>
#include <rv/app/world_build.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/math.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/static_instance_group.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/pick_index.hpp>

#include "world_notice_panel.hpp"

namespace rv::app {

class AppContext;
class CollectionInstance;
class CoverLoader;
class ProductUi;
class SceneGraphics;

class WorldSession {
public:
    /// What the session asks of the application at a frame boundary.
    struct SceneHost {
        /// Shows `description` in place of the scene shown (its models from the asset root), the walker at its camera
        /// (or at --spawn). The session's collection is gone by then (the registries are cleared). Errors: the scene
        /// cannot be made.
        std::function<Status(scene::SceneDescription description)> showScene;
        /// Shows the test room (m0_testroom) in place of the scene shown.
        std::function<Status()> showTestRoom;
    };

    struct Options {
        WorldPaths paths;
        catalog::SystemName system;
        std::string writer;
        bool lockstep = false;
        /// When the process was created (FILETIME ticks): the run report's times count from it.
        std::uint64_t processCreated = 0;
    };

    /// How long a lockstep run waits between two frames, at most.
    static constexpr std::chrono::seconds kLockstepWaitLimit{10};

    /// The room of the run: made by a flow on `pool` (`decided` set: no flow, that result at once, the test room the
    /// application already shows for want of a catalog).
    WorldSession(AppContext& context, Options options, core::WorkerPool& pool, SceneGraphics& graphics, SceneHost host,
                 std::optional<WorldResult> decided = std::nullopt);
    ~WorldSession();
    WorldSession(const WorldSession&) = delete;
    WorldSession& operator=(const WorldSession&) = delete;

    /// The empty room of the system's template (the shell shown while the room is made).
    [[nodiscard]] static Expected<scene::SceneDescription> shellScene(const std::filesystem::path& assetRoot,
                                                                      const catalog::SystemName& system);

    /// One step, after the product UI's update or idle wake (its catalog polled): the flow moved on, its result
    /// committed (the scene replaced: call it at a frame boundary), the notice updated at the run's time
    /// `timeSeconds`; and the covers' round (within the frame's budget, or a larger one in an idle wake).
    void update(ProductUi& product, bool idleWake, double timeSeconds);
    /// The hover, once per frame: `ray` from the reticle while the world has the input, nothing otherwise (the hover
    /// lets go at once). The hovered item is outlined.
    void hover(const std::optional<core::Ray>& ray);
    /// A frame was rendered (the lockstep wait starts again).
    void frameRendered();

    /// The room's static groups for the view (none before its collection exists).
    [[nodiscard]] std::span<const gl::StaticInstanceGroup* const> staticGroups() const noexcept;
    /// A lockstep run must wait before its next frame: the room or its layers are on their way (within the limit).
    [[nodiscard]] bool waits() const;
    /// The collection goes (before the application replaces the scene and clears the registries).
    void dropCollection();

    /// The end of the run: the world and residency records.
    void record(RunReport& report) const;

private:
    void commit(WorldResult result);
    /// Shows `notice` from the next showNotice() on: for world_notice::kShownSeconds of the run's time (`timed`), or
    /// until another replaces it.
    void setNotice(WorldNoticeContent notice, bool timed);
    void showNotice(ProductUi& product, double timeSeconds);
    [[nodiscard]] std::optional<double> sinceProcessStart(std::chrono::system_clock::time_point at) const;

    AppContext& m_context;
    Options m_options;
    core::WorkerPool& m_pool;
    SceneGraphics& m_graphics;
    SceneHost m_host;
    std::unique_ptr<WorldFlow> m_flow;
    std::optional<WorldResult> m_decided;
    std::unique_ptr<CollectionInstance> m_collection;
    std::unique_ptr<CoverLoader> m_loader;
    world::PickIndex m_pickIndex;
    world::HoverTracker m_hover;
    /// What showed, the notice and when it was set (seconds of the run's time; it hides after its time unless the
    /// shell shows), the record.
    std::string m_state = "generating";
    WorldNoticeContent m_notice;
    std::optional<double> m_noticeSince;
    bool m_noticeTimed = false;
    WorldRecord m_record;
    std::optional<std::chrono::system_clock::time_point> m_shownAt;
    bool m_coverPhaseOpen = false;
    std::optional<std::chrono::steady_clock::time_point> m_waitSince;
    bool m_waitWarned = false;
};

} // namespace rv::app
