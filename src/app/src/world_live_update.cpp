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
    spdlog::info("Frame {}: the catalog was refreshed; the floor follows it once nothing else is on its way",
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
    flow.startRoom = m_options.startRoom;
    flow.writer = m_options.writer;
    flow.wake = m_context.wakeCallback();
    flow.preview = false; // the floor shows already: the building job at once
    m_flow = std::make_unique<WorldFlow>(std::move(flow), m_pool);
    m_liveFlow = true;
    m_flow->update(product.catalog());
    spdlog::info("Frame {}: the building's job runs again for the refreshed catalog", m_context.frameIndex());
}

void WorldSession::keepShown(const WorldBuild& build) {
    m_shown = shownFloorOf(build);
    // The card's other versions of each title (I3 step 14a), from the plan shown.
    m_otherVersions = build.plan ? otherVersionCounts(*build.plan) : std::map<std::uint64_t, std::uint32_t>{};
    // Where each game stands, for "Mostra nella stanza" (I3 step 15a).
    m_launch.setPlan(build.plan ? &*build.plan : nullptr);
    m_versionGroups = build.plan ? VersionGroups::of(*build.plan) : VersionGroups{}; // Informazioni (I3 step 14b)
}

void WorldSession::releaseHero() {
    if (m_heroShown && m_heroShown->room < m_rooms.size() && m_rooms[m_heroShown->room].collection) {
        if (Status restored = m_rooms[m_heroShown->room].collection->setFrontHidden(m_heroShown->item, false);
            !restored) {
            spdlog::error("World: the hero's front: {}", restored.error().toString());
        }
    }
    m_heroShown.reset();
    m_heroItem.reset();
    m_dynamicItems.clear();
}

void WorldSession::releaseInteraction() {
    m_interaction->clear();
    for (FloorRoom& room : m_rooms) {
        if (!room.collection) {
            continue;
        }
        // clear() leaves the collections' records as they are: the items it had pulled out go back to their slots.
        for (const world::PickId id : room.collection->hiddenItems()) {
            if (Status shown = room.collection->setHidden(id, false); !shown) {
                spdlog::error("World: an item back in its slot: {}", shown.error().toString());
            }
        }
        if (Status cleared = room.collection->setHovered(world::kNoPick); !cleared) {
            spdlog::error("World: the hover outline: {}", cleared.error().toString());
        }
    }
}

bool WorldSession::applyItems(WorldBuild& build, const LiveUpdatePlan& plan, WorldLiveUpdateRecord& record) {
    // Each room's collection applied in place: the same rooms, each with its collection (a room that gained its first
    // item, or lost its last, is a commit of the floor).
    if (m_rooms.size() != build.rooms.size()) {
        return false;
    }
    for (std::size_t index = 0; index < m_rooms.size(); ++index) {
        if (m_rooms[index].id != build.rooms[index].room ||
            (m_rooms[index].collection != nullptr) != !build.rooms[index].layout.items.empty()) {
            return false;
        }
    }
    // The hero's front goes back to the static path (the items' indices change); the next hover shows it again. The
    // interaction (I3 step 13a) lets go of its items: a pulled-out item back in its slot, Focus left; it starts again
    // over the new layouts below.
    releaseHero();
    releaseInteraction();
    m_record.pickTargets = 0;
    m_record.items = 0;
    m_record.clusters = 0;
    m_record.skipped = 0;
    m_record.staticRecords = 0;
    for (std::size_t index = 0; index < m_rooms.size(); ++index) {
        FloorRoom& room = m_rooms[index];
        const world::CollectionLayout& layout = build.rooms[index].layout;
        m_record.items += layout.items.size();
        m_record.clusters += layout.clusters.size();
        m_record.skipped += layout.skipped.size();
        if (!room.collection) {
            continue;
        }
        auto updated =
            room.collection->applyLayout(layout, m_graphics, m_context.device().caps().uniformBufferOffsetAlignment);
        if (!updated) {
            spdlog::error("World: the items of {} cannot be updated in place: {}; the floor is committed again",
                          room.id.toString(), updated.error().toString());
            return false;
        }
        record.arraysGrown = record.arraysGrown || updated->grown;
        record.arrayAdded = record.arrayAdded || updated->arraysAdded;
        // The hover, the pull-out and Focus over the new layout (its pick targets and D-pad steps; the pick ids of the
        // items that stood are theirs still).
        m_record.pickTargets += m_interaction->addRoom(layout, room.collection.get());
        m_record.staticRecords += room.collection->group().layout().instances().size();
    }
    syncCollections();
    if (const std::optional<std::size_t> eye = eyeRoom()) {
        m_interaction->setActiveRoom(m_rooms[*eye].interactionRoom);
    } else {
        m_interaction->setActiveRoom(std::nullopt);
    }
    countPlan(build, m_record);
    record.items = m_record.items;
    record.staticRecords = m_record.staticRecords;
    if (m_heroes) {
        for (const catalog::GameId& game : plan.coversChanged) {
            m_heroes->forget(game);
        }
    }
    // The covers: the items that stood adopted (their pictures uploaded again only when they changed), the new ones
    // made.
    startCoverLoader(build);
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
        // No floor showed (the shell, the test room): the refreshed catalog's floor is committed as at the start.
        record.kind = std::string(toString(LiveUpdateKind::Rebuild));
        record.reason = "no room showed";
        commit(std::move(result));
        finish();
        return;
    }
    if (result.testRoom || !result.build) {
        // A live update never takes away a floor that shows (ADR-0010 §11).
        record.kind = std::string(toString(LiveUpdateKind::Kept));
        record.reason = result.reason;
        spdlog::warn("World: the refreshed catalog gives no building ({}); the floor shown stays", result.reason);
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
        spdlog::warn("World: the refreshed catalog's floor cannot be shown ({}); the floor shown stays", plan.reason);
        finish();
        return;
    }
    recordBuild(build);
    switch (plan.kind) {
    case LiveUpdateKind::Unchanged:
        break;
    case LiveUpdateKind::Textures:
        if (m_loader && [&] {
                for (const RoomCollection& room : build.rooms) {
                    const ShownCollection* before = m_shown->find(room.room);
                    if (before == nullptr || before->covers != room.covers) {
                        return false;
                    }
                }
                return true;
            }()) {
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
        startCoverLoader(build);
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
        if (build.startRoomMissing) {
            spdlog::warn("World: the refreshed building has no start room; the floor shown stays");
            break;
        }
        if (showFloor(build, true)) {
            record.items = m_record.items;
            record.staticRecords = m_record.staticRecords;
        }
        break;
    }
    if (!build.notice.empty()) {
        setNotice(WorldNoticeContent{build.notice, {}, noticeSystem()}, true);
    }
    m_record.state = m_state;
    finish();
}

} // namespace rv::app
