#include "world_start.hpp"

#include <utility>

#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/app/world_reconcile.hpp>

#include "collection_instance.hpp"
#include "cover_loader.hpp"

namespace rv::app {

void WorldSession::commitPreview(WorldBuild preview) {
    const Clock::time_point start = Clock::now();
    if (!preview.plan || !preview.scene || !preview.sceneData || !preview.layout) {
        // The room waits for the snapshot (the shell and its notice stay); the flow asks for it at once.
        spdlog::warn("World: the stored plan's room is not shown before the catalog's snapshot: {}", preview.reason);
        return;
    }
    recordBuild(preview);
    if (!showRoom(preview, false)) {
        return;
    }
    m_record.firstFrame = m_context.frameIndex();
    WorldPreviewRecord record;
    record.frame = m_context.frameIndex();
    record.committedAtMs = sinceProcessStart(std::chrono::system_clock::now());
    record.buildMs = preview.milliseconds;
    record.planDigest = preview.planDigest;
    record.items = preview.layout->items.size();
    record.itemsKnown = preview.itemsRead;
    record.commitMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    m_record.preview = record;
    m_record.state = m_state;
    // The room is usable: no notice while the snapshot is asked for.
    setNotice(WorldNoticeContent{std::string(world_notice::kNone), {}, m_options.system.str()}, true);
    spdlog::info("Frame {}: the stored plan's room is shown before the catalog's snapshot ({} item(s), {} with their "
                 "pictures from the cache); committed in {:.1f} ms",
                 m_context.frameIndex(), record.items, record.itemsKnown, *record.commitMs);
    preview.scene.reset();
    m_shownPreview = std::move(preview);
    m_previewAwaitingFrame = true;
}

void WorldSession::reconcile(WorldResult result) {
    const Clock::time_point start = Clock::now();
    const WorldBuild shown = std::move(*m_shownPreview);
    m_shownPreview.reset();
    if (result.testRoom || !result.build) {
        commit(std::move(result));
        return;
    }
    WorldBuild& build = *result.build;
    const ReconcilePlan plan = planReconciliation(shown, build);
    recordBuild(build);
    WorldReconcileRecord record;
    record.kind = std::string(toString(plan.kind));
    record.frame = m_context.frameIndex();
    record.kept = plan.difference.kept;
    record.moved = plan.difference.moved;
    record.flagsChanged = plan.difference.flagsChanged;
    record.added = plan.difference.added;
    record.removed = plan.difference.removed;
    switch (plan.kind) {
    case Reconciliation::Shell: {
        // A plan that cannot be used now (ADR-0010 §11): the empty room and its notice, as without a stored plan.
        dropCollection();
        if (auto shell = shellScene(m_options.paths.assetRoot, m_options.system)) {
            if (Status shownShell = m_host.showScene(std::move(*shell)); !shownShell) {
                spdlog::error("World: the room's shell cannot be shown: {}", shownShell.error().toString());
            }
        } else {
            spdlog::error("World: the room's shell cannot be made: {}", shell.error().toString());
        }
        m_state = "shell";
        m_record.state = m_state;
        setNotice(WorldNoticeContent{build.notice.empty() ? std::string(world_notice::kFailed) : build.notice,
                                     {},
                                     m_options.system.str()},
                  false);
        break;
    }
    case Reconciliation::Rebuild: {
        // I3 step 15b: another collection of the same static part has its items applied in place (nothing that stood
        // is rebuilt or loaded again); else committed again, the walker where it stands. Either way the items that
        // stood keep their places (the incremental rules move none).
        const LiveUpdatePlan live =
            m_shown ? planLiveUpdate(*m_shown, build) : LiveUpdatePlan{};
        WorldLiveUpdateRecord applied;
        if (live.kind == LiveUpdateKind::Items && applyItems(build, live, applied)) {
            record.kind = std::string(toString(LiveUpdateKind::Items));
            finishCommit(build, start);
        } else if (showRoom(build, true)) {
            finishCommit(build, start);
        }
        break;
    }
    case Reconciliation::Textures:
        // Nothing moves and nothing is rebuilt: the covers are served again when they changed or their sources can
        // be checked now, and the hero CRT gets the room's games.
        record.coversReloaded = plan.reloadCovers;
        if (plan.reloadCovers) {
            startCoverLoader(build.covers, build.mediaRoot, build.library);
        }
        keepShown(build); // I3 step 15b: the next live update compares with the snapshot's room
        if (m_host.setScreenGames) {
            m_host.setScreenGames(build.screenGames);
        }
        finishCommit(build, start);
        break;
    }
    m_record.reconcile = record;
    spdlog::info("Frame {}: the stored plan's room reconciled with the catalog's snapshot ({}): {} item(s) kept, {} "
                 "moved, {} with other flags, {} added, {} removed{}",
                 m_context.frameIndex(), record.kind, record.kept, record.moved, record.flagsChanged, record.added,
                 record.removed, record.coversReloaded ? "; the covers served again" : "");
}

} // namespace rv::app
