#include "world_start.hpp"

#include <utility>

#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/app/world_live_plan.hpp>
#include <rv/app/world_reconcile.hpp>

#include "collection_instance.hpp"
#include "cover_loader.hpp"

namespace rv::app {

void WorldSession::commitPreview(WorldBuild preview) {
    const Clock::time_point start = Clock::now();
    if (!preview.plan || !preview.scene) {
        // The floor waits for the building (the shell and its notice stay); the flow asks for it at once.
        spdlog::warn("World: the stored plan's floor is not shown before the building's result: {}",
                     preview.startRoomMissing ? std::string("the start room is not in it") : preview.reason);
        return;
    }
    recordBuild(preview);
    if (!showFloor(preview, false)) {
        return;
    }
    m_record.firstFrame = m_context.frameIndex();
    WorldPreviewRecord record;
    record.frame = m_context.frameIndex();
    record.committedAtMs = sinceProcessStart(std::chrono::system_clock::now());
    record.buildMs = preview.milliseconds;
    record.planDigest = preview.planDigest;
    record.items = preview.itemCount();
    record.itemsKnown = preview.itemsRead;
    record.commitMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    m_record.preview = record;
    m_record.state = m_state;
    // The floor is usable: no notice while the building is made.
    setNotice(WorldNoticeContent{std::string(world_notice::kNone), {}, noticeRoom()}, true);
    spdlog::info("Frame {}: the stored plan's floor {} is shown before the building's result ({} item(s), {} with "
                 "their pictures from the cache); committed in {:.1f} ms",
                 m_context.frameIndex(), preview.floor, record.items, record.itemsKnown, *record.commitMs);
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
    if (build.startRoomMissing) {
        commit(std::move(result));
        return;
    }
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
        // A plan that cannot be used now (ADR-0010 §11): the empty shell and its notice, as without a stored plan.
        dropCollection();
        if (auto shell = shellScene(m_options.paths.assetRoot, m_options.startRoom)) {
            if (Status shownShell = m_host.showScene(std::move(*shell), SceneShow{}); !shownShell) {
                spdlog::error("World: the room's shell cannot be shown: {}", shownShell.error().toString());
            }
        } else {
            spdlog::error("World: the room's shell cannot be made: {}", shell.error().toString());
        }
        m_state = "shell";
        m_record.state = m_state;
        setNotice(WorldNoticeContent{build.notice.empty() ? std::string(world_notice::kFailed) : build.notice,
                                     {},
                                     noticeRoom()},
                  false);
        break;
    }
    case Reconciliation::Rebuild: {
        // I3 step 15b, I4 step 6b: the same static part has the rooms whose items changed applied in place (nothing
        // that stood is rebuilt or loaded again, the other rooms untouched); else the floor is committed again, the
        // walker kept in the frame of the area that holds it. Either way the items that stood keep their places (the
        // incremental rules move none).
        WorldLiveUpdateRecord applied;
        if (plan.live.kind == LiveUpdateKind::Items && applyRooms(build, plan.live, applied)) {
            record.kind = std::string(toString(LiveUpdateKind::Items));
            record.rooms = roomRecords(plan.live, false);
            finishCommit(build, start);
        } else if (showFloor(build, true, &record.walker)) {
            record.rooms = roomRecords(plan.live, true);
            finishCommit(build, start);
        }
        break;
    }
    case Reconciliation::Textures: {
        // Room by room (I4 step 6b): an unchanged room keeps its collection and its pictures, a room whose covers
        // changed (or whose sources can be checked now) has them served again, uploading only what changed; the hero
        // CRT gets its room's games.
        record.coversReloaded = plan.reloadCovers;
        WorldLiveUpdateRecord applied;
        static_cast<void>(applyRooms(build, plan.live, applied));
        record.rooms = roomRecords(plan.live, false);
        finishCommit(build, start);
        break;
    }
    }
    m_record.reconcile = record;
    spdlog::info("Frame {}: the stored plan's floor reconciled with the building's result ({}): {} item(s) kept, {} "
                 "moved, {} with other flags, {} added, {} removed{}",
                 m_context.frameIndex(), record.kind, record.kept, record.moved, record.flagsChanged, record.added,
                 record.removed, record.coversReloaded ? "; the covers served again" : "");
}

} // namespace rv::app
