#include "world_live_update.hpp"

#include <utility>

#include <spdlog/spdlog.h>

#include <rv/app/app_catalog.hpp>
#include <rv/app/app_logic.hpp>
#include <rv/app/world_card_ui.hpp>
#include <rv/render/gl_device.hpp>

#include "collection_instance.hpp"
#include "cover_loader.hpp"
#include "hero_textures.hpp"
#include "product_ui.hpp"

namespace rv::app {

void WorldSession::requestLiveUpdate() {
    if (!m_liveRequested) {
        m_liveRequestFrame = m_context.frameIndex();
        m_liveRequestedAt = Clock::now();
    }
    m_liveRequested = true;
    spdlog::info("Frame {}: the catalog was refreshed; the room follows it once nothing else is on its way",
                 m_context.frameIndex());
}

void WorldSession::startLiveFlow(ProductUi& product) {
    // One flow at a time; the persisted start's reconciliation first (its snapshot may already follow the refresh, the
    // live update then finds nothing to do).
    if (!m_liveRequested || m_decided || m_shownPreview || m_previewAwaitingFrame ||
        (m_flow && m_flow->phase() != WorldPhase::Done)) {
        return;
    }
    m_liveRequested = false;
    WorldFlow::Options flow;
    flow.paths = m_options.paths;
    flow.system = m_options.system;
    flow.writer = m_options.writer;
    flow.wake = m_context.wakeCallback();
    flow.preview = false; // the room shows already: the snapshot at once
    m_flow = std::make_unique<WorldFlow>(std::move(flow), m_pool);
    m_liveFlow = true;
    m_flow->update(product.catalog());
    spdlog::info("Frame {}: the room's job runs again for the refreshed catalog", m_context.frameIndex());
}

void WorldSession::keepShown(const WorldBuild& build) {
    m_shown = shownRoomOf(build);
    // The card's other versions of each title (I3 step 14a), from the plan shown.
    m_otherVersions = build.plan ? otherVersionCounts(*build.plan) : std::map<std::uint64_t, std::uint32_t>{};
    // Where each game stands, for "Mostra nella stanza" (I3 step 15a).
    m_launch.setPlan(build.plan ? &*build.plan : nullptr);
    m_versionGroups = build.plan ? VersionGroups::of(*build.plan) : VersionGroups{}; // Informazioni (I3 step 14b)
}

void WorldSession::releaseHero() {
    if (m_collection && m_heroShown) {
        if (Status restored = m_collection->setFrontHidden(*m_heroShown, false); !restored) {
            spdlog::error("World: the hero's front: {}", restored.error().toString());
        }
    }
    m_heroShown.reset();
    m_heroItem.reset();
    m_dynamicItems.clear();
}

void WorldSession::releaseInteraction() {
    m_interaction->clear();
    if (m_collection) {
        // clear() leaves the collection's records as they are: the items it had pulled out go back to their slots.
        for (const world::PickId id : m_collection->hiddenItems()) {
            if (Status shown = m_collection->setHidden(id, false); !shown) {
                spdlog::error("World: an item back in its slot: {}", shown.error().toString());
            }
        }
        if (Status cleared = m_collection->setHovered(world::kNoPick); !cleared) {
            spdlog::error("World: the hover outline: {}", cleared.error().toString());
        }
    }
}

bool WorldSession::applyItems(WorldBuild& build, const LiveUpdatePlan& plan, WorldLiveUpdateRecord& record) {
    if (!m_collection || !build.layout || build.layout->items.empty()) {
        return false;
    }
    // The hero's front goes back to the static path (the items' indices change); the next hover shows it again. The
    // interaction (I3 step 13a) lets go of its items: a pulled-out item back in its slot, Focus left; it starts again
    // over the new layout below.
    releaseHero();
    releaseInteraction();
    auto updated =
        m_collection->applyLayout(*build.layout, m_graphics, m_context.device().caps().uniformBufferOffsetAlignment);
    if (!updated) {
        spdlog::error("World: the room's items cannot be updated in place: {}; the room is committed again",
                      updated.error().toString());
        return false;
    }
    record.arraysGrown = updated->grown;
    record.arrayAdded = updated->arraysAdded;
    // The hover, the pull-out and Focus over the new layout (its pick targets and D-pad steps; the pick ids of the
    // items that stood are theirs still).
    m_record.pickTargets = m_interaction->setRoom(*build.layout, m_collection.get());
    countPlan(build, m_record);
    m_record.items = build.layout->items.size();
    m_record.clusters = build.layout->clusters.size();
    m_record.skipped = build.layout->skipped.size();
    m_record.staticRecords = m_collection->group().layout().instances().size();
    record.items = m_record.items;
    record.staticRecords = m_record.staticRecords;
    if (m_heroes) {
        for (const catalog::GameId& game : plan.coversChanged) {
            m_heroes->forget(game);
        }
    }
    // The covers: the items that stood adopted (their pictures uploaded again only when they changed), the new ones
    // made.
    startCoverLoader(build.covers, build.mediaRoot, build.library);
    if (plan.screenGamesChanged && m_host.setScreenGames) {
        m_host.setScreenGames(build.screenGames);
    }
    keepShown(build);
    return true;
}

void WorldSession::applyLive(WorldResult result) {
    const Clock::time_point start = Clock::now();
    WorldLiveUpdateRecord record;
    record.trigger = "refresh";
    record.requestFrame = m_liveRequestFrame;
    record.frame = m_context.frameIndex();
    if (m_liveRequestedAt) {
        record.requestToApplyMs = std::chrono::duration<double, std::milli>(start - *m_liveRequestedAt).count();
        m_liveRequestedAt.reset();
    }
    const auto finish = [&] {
        record.applyMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        spdlog::info("Frame {}: the room's live update ({}): {} item(s) added, {} removed, {} kept, {} moved, {} with "
                     "other flags, {} cover(s) changed; applied in {:.1f} ms{}",
                     record.frame, record.kind, record.added, record.removed, record.kept, record.moved,
                     record.flagsChanged, record.coversChanged, *record.applyMs,
                     record.reason.empty() ? std::string() : " (" + record.reason + ")");
        m_record.liveUpdates.push_back(std::move(record));
    };
    if (result.build) {
        const WorldBuild& build = *result.build;
        record.outcome = std::string(toString(build.outcome));
        record.rebuild = build.changes ? std::string(world::toString(build.changes->rebuild)) : std::string();
        record.planWritten = build.written;
        record.buildMs = build.milliseconds;
    }
    if (!m_shown || m_state != "room") {
        // No room showed (the shell, the test room): the refreshed catalog's room is committed as at the start.
        record.kind = std::string(toString(LiveUpdateKind::Rebuild));
        record.reason = "no room showed";
        commit(std::move(result));
        finish();
        return;
    }
    if (result.testRoom || !result.build) {
        // A live update never takes away a room that shows (ADR-0010 §11).
        record.kind = std::string(toString(LiveUpdateKind::Kept));
        record.reason = result.reason;
        spdlog::warn("World: the refreshed catalog gives no room ({}); the room shown stays", result.reason);
        finish();
        return;
    }
    WorldBuild& build = *result.build;
    const LiveUpdatePlan plan = planLiveUpdate(*m_shown, build);
    record.kind = std::string(toString(plan.kind));
    record.reason = plan.reason;
    record.kept = plan.difference.kept;
    record.moved = plan.difference.moved;
    record.flagsChanged = plan.difference.flagsChanged;
    record.added = plan.difference.added;
    record.removed = plan.difference.removed;
    record.coversChanged = plan.coversChanged.size();
    if (plan.kind == LiveUpdateKind::Kept) {
        spdlog::warn("World: the refreshed catalog's room cannot be shown ({}); the room shown stays", plan.reason);
        finish();
        return;
    }
    recordBuild(build);
    switch (plan.kind) {
    case LiveUpdateKind::Unchanged:
        break;
    case LiveUpdateKind::Textures:
        if (m_loader && build.covers == m_shown->covers) {
            // Only the media root is known now: the loader's retry after the refresh (I3 step 12, followRefresh) serves
            // again the items whose fronts did not come from their covers.
            keepShown(build);
            break;
        }
        // ADR-0010 §10's NoRebuild: the textures and the spine text swapped, nothing else.
        releaseHero();
        if (m_heroes) {
            for (const catalog::GameId& game : plan.coversChanged) {
                m_heroes->forget(game);
            }
        }
        startCoverLoader(build.covers, build.mediaRoot, build.library);
        if (plan.screenGamesChanged && m_host.setScreenGames) {
            m_host.setScreenGames(build.screenGames);
        }
        keepShown(build);
        break;
    case LiveUpdateKind::Items:
        if (applyItems(build, plan, record)) {
            break;
        }
        record.kind = std::string(toString(LiveUpdateKind::Rebuild));
        record.reason = "the items could not be applied in place";
        [[fallthrough]];
    case LiveUpdateKind::Rebuild:
    case LiveUpdateKind::Kept:
        if (showRoom(build, true)) {
            record.items = m_record.items;
            record.staticRecords = m_record.staticRecords;
        }
        break;
    }
    if (!build.notice.empty()) {
        setNotice(WorldNoticeContent{build.notice, {}, m_options.system.str()}, true);
    }
    m_record.state = m_state;
    finish();
}

} // namespace rv::app
