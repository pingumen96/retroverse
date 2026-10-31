// The regenerations and the undo in the app (I4b step 6; ADR-0015 §5, §6): WorldSession's side
// (world_regenerate_state.hpp's file comment): the directory's "Rigenera stanza / piano / mondo" and "Annulla ultima
// modifica" and the update's needs-confirmation notice opening the preview (regenerate_preview.rml), its dry run and
// the commit of the choice on the worker pool (world_regenerate.hpp), the floor shown again from the plan stored after
// it (the rooms it changed rebuilt alone when the floor's other rooms and its hall stand as they did, step 4's path;
// else room by room when its static part stands, else committed again with the walker kept), the walker of a
// regenerated room at its doorway.

#include "world_session.hpp"

#include <algorithm>
#include <exception>
#include <set>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/app_catalog.hpp>
#include <rv/app/app_logic.hpp>
#include <rv/app/directory_data.hpp>
#include <rv/app/world_card_ui.hpp>
#include <rv/core/version.hpp>
#include <rv/world/building.hpp>

#include "product_ui.hpp"
#include "world_regenerate_state.hpp"

namespace rv::app {

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// The panel's name of `room` (its kind and, for a console room, its system's full name).
[[nodiscard]] ui::RegenerateScopeTitle roomTitleOf(const world::RoomId& room, AppCatalog& catalog) {
    ui::RegenerateScopeTitle title;
    switch (room.kind()) {
    case world::RoomIdKind::System: {
        title.roomKind = "console";
        const std::string fullName = catalog.systemFullName(room.system());
        title.roomTitle = fullName.empty() ? room.system().str() : fullName;
        break;
    }
    case world::RoomIdKind::Shared:
        title.roomKind = "shared";
        break;
    case world::RoomIdKind::Arcade:
        title.roomKind = "arcade";
        break;
    }
    return title;
}

void copyCounts(const ui::RegeneratePreviewContent& content, WorldRegenerationRecord& record) {
    record.roomsRegenerated = content.roomsRegenerated;
    record.roomsAdded = content.roomsAdded;
    record.roomsRemoved = content.roomsRemoved;
    record.roomsMoved = content.roomsMoved;
    record.gamesMoved = content.gamesMoved;
    record.faceOutEntering = content.faceOutEntering;
    record.faceOutLeaving = content.faceOutLeaving;
    record.newlyPlaced = content.newlyPlaced;
    record.newlyUnplaced = content.newlyUnplaced;
    record.fixturesAdded = content.fixturesAdded;
    record.fixturesRemoved = content.fixturesRemoved;
    record.fixturesMoved = content.fixturesMoved;
    record.pinsKept = content.pinsKept;
    record.conflicts = content.conflictCount;
    record.overrides = content.overrides;
    record.changed = content.changed;
}

/// What a commit's job builds the floor shown from (the session's state when the choice is made).
struct FloorJob {
    WorldPaths paths;
    WorldBuild skeleton;
    FloorSources sources;
    std::shared_ptr<WorldCatalogReader> reader;
    /// The floor shown and the plan it shows (none: the floor is built whole), the synthetic scene digest of the
    /// rooms rebuilt alone.
    std::shared_ptr<const RegenerateShownFloor> shown;
    std::shared_ptr<const world::WorldPlan> before;
    std::string sceneDigest;
};

/// The floor to show for the plan stored after a commit (`plan`, its digest, the rooms' items written again), on the
/// pool: the rooms it changed rebuilt alone when the floor's other rooms and its hall stand as they did (I4b step 6's
/// review: step 4's path; the live update's decision made here as the main thread will make it), else the floor built
/// from the plan as a ride's destination.
void buildCommittedFloor(const FloorJob& job, const std::shared_ptr<const world::WorldPlan>& plan,
                         const std::string& digest, std::map<catalog::SystemName, RoomItems> items,
                         RegenerateJobAnswer& answer) {
    if (!plan) {
        return;
    }
    const Clock::time_point start = Clock::now();
    auto sources = std::make_shared<FloorSources>(job.sources);
    sources->items = std::move(items);
    if (job.reader) {
        sources->lastPlayed = catalogLastPlayed(*job.reader);
    }
    answer.fallback = sources;
    const double lastPlayedMs = millisecondsSince(start);
    if (job.shown && job.before) {
        // The plan as the floor shows it (a stored building's ground floor on the hub, as buildFloor() draws it).
        world::WorldPlan drawn = *plan;
        if (auto building = world::loadBuildingData(job.paths.assetRoot)) {
            static_cast<void>(world::placeOnHub(drawn.rooms, building->layout));
        }
        Clock::time_point part = Clock::now();
        if (auto rooms = roomsChangedAlone(*job.before, drawn, job.shown->shown.floor)) {
            const double compareMs = millisecondsSince(part);
            part = Clock::now();
            WorldBuild alone = templateOf(*job.shown);
            alone.plan = std::move(drawn);
            alone.planDigest = digest;
            alone.reason = "a regeneration's rooms";
            Status rebuilt = rebuildRoomsAlone(job.paths, alone, *rooms, *sources);
            const double roomsMs = millisecondsSince(part);
            part = Clock::now();
            std::vector<RoomStaticPart> statics;
            for (const world::RoomId& room : *rooms) {
                if (!rebuilt) {
                    break;
                }
                auto staticPart = roomStaticPart(alone, *alone.plan, room);
                if (!staticPart) {
                    rebuilt = propagate(staticPart.error());
                    break;
                }
                statics.push_back(std::move(*staticPart));
            }
            if (rebuilt) {
                alone.scene.emplace(); // the scene shown, its rooms' parts replaced: a stand-in for the decision
                alone.sceneDigest = statics.empty() ? job.shown->shown.sceneDigest : job.sceneDigest;
                ShownFloor probe = job.shown->shown;
                probe.sceneDigest = alone.sceneDigest;
                const LiveUpdatePlan update = planLiveUpdate(probe, alone, true);
                if (update.kind != LiveUpdateKind::Rebuild && update.kind != LiveUpdateKind::Kept) {
                    answer.floorMs = millisecondsSince(start);
                    spdlog::info("Regeneration: {} room(s) of the floor {} rebuilt alone in {:.0f} ms (the CRTs' plays "
                                 "{:.0f}, the rooms compared {:.0f}, their collections {:.0f}, their static parts and "
                                 "the decision {:.0f})",
                                 rooms->size(), alone.floor, answer.floorMs, lastPlayedMs, compareMs, roomsMs,
                                 millisecondsSince(part));
                    answer.build = std::move(alone);
                    answer.statics = std::move(statics);
                    answer.alone = true;
                    return;
                }
                spdlog::info("Regeneration: the rooms rebuilt alone cannot be shown in place ({}); the floor is built",
                             update.reason);
            } else {
                spdlog::warn("Regeneration: the rooms cannot be rebuilt alone ({}); the floor is built",
                             rebuilt.error().toString());
            }
        }
    }
    WorldBuild build = job.skeleton;
    build.plan = *plan;
    build.planDigest = digest;
    build.reason = "a regeneration's floor";
    buildFloor(job.paths, build, *sources);
    answer.floorMs = millisecondsSince(start);
    if (!build.plan || !build.scene) {
        answer.buildError = "the floor cannot be built: " + build.reason;
        return;
    }
    answer.build = std::move(build);
}

} // namespace

bool WorldSession::regenerating() const noexcept {
    return m_regenerations &&
           (m_regenerations->request.has_value() ||
            (m_regenerations->job != RegenerateJob::None && m_regenerations->job != RegenerateJob::Prewarm));
}

bool WorldSession::regenerationWaits() const noexcept {
    // A lockstep run waits for the preview's and the commit's jobs (a prewarm is no reason to wait).
    return m_regenerations &&
           (m_regenerations->job == RegenerateJob::Preview || m_regenerations->job == RegenerateJob::Commit);
}

bool WorldSession::regenerationAllowed(const ProductUi& product) const {
    return m_state == "room" && m_shown && m_planState && !m_ride && !m_edit && !m_retargetPending &&
           !m_retargetResult && !m_shownPreview && !m_previewAwaitingFrame && !launchHolds(product) &&
           !(m_flow && m_flow->phase() != WorldPhase::Done) && !regenerating();
}

bool WorldSession::confirmationOffered() const {
    // While the notice shows (it hides after its time), whatever its preview did meanwhile.
    return m_regenerations && m_regenerations->confirmationRoom &&
           m_notice.notice == world_notice::kNeedsConfirmation && noticeShowing();
}

void WorldSession::openConfirmationPreview(ProductUi& product) {
    if (!confirmationOffered()) {
        return;
    }
    const world::RoomId room = *m_regenerations->confirmationRoom;
    spdlog::info("Frame {}: the notice's preview of Rigenera stanza for {}", m_context.frameIndex(), room.toString());
    beginRegeneration(product, ui::RegenerateKind::Room, world::RegenerationScope::ofRoom(room), "notice",
                      std::nullopt);
}

void WorldSession::submitRegenerateJob(RegenerateJob job,
                                       std::function<RegenerateJobAnswer(const core::JobContext&)> work) {
    WorldRegenerations& state = *m_regenerations;
    m_pool.cancel(state.generation);
    const std::uint64_t generation = state.generation.current();
    // The box names the new job's generation: a cancelled job's late answer is refused (I4b step 6's review).
    state.box->mailbox.reset(generation);
    state.job = job;
    state.asked = Clock::now();
    const RegenerateJobAnswer::Kind kind =
        job == RegenerateJob::Commit ? RegenerateJobAnswer::Kind::Commit : RegenerateJobAnswer::Kind::Preview;
    core::JobOptions options;
    options.priority = core::WorkerPool::kHighestPriority; // the world's jobs (ADR-0010 §12)
    options.generation = &state.generation;
    const Status submitted = m_pool.submit(
        [box = state.box, generation, kind, work = std::move(work)](const core::JobContext& context) {
            if (context.cancelled()) {
                return;
            }
            const Clock::time_point start = Clock::now();
            RegenerateJobAnswer answer;
            try {
                answer = work(context);
            } catch (const std::exception& error) {
                answer = RegenerateJobAnswer{};
                answer.kind = kind;
                answer.failure = std::string(regenerate_failure::kFailed);
                answer.error = std::string("the job stopped: ") + error.what();
            } catch (...) {
                answer = RegenerateJobAnswer{};
                answer.kind = kind;
                answer.failure = std::string(regenerate_failure::kFailed);
                answer.error = "the job stopped: an unknown exception";
            }
            answer.milliseconds = millisecondsSince(start);
            if (context.cancelled()) {
                return; // a newer job was asked: its answer is the one waited for
            }
            if (box->mailbox.post(generation, std::move(answer))) {
                std::function<void()> wake;
                {
                    const std::lock_guard lock(box->mutex);
                    wake = box->wake;
                }
                if (wake) {
                    wake();
                }
            }
        },
        options);
    if (!submitted) {
        spdlog::error("Regeneration: a job cannot be submitted: {}", submitted.error().toString());
        RegenerateJobAnswer failed;
        failed.kind = kind;
        failed.failure = std::string(regenerate_failure::kFailed);
        failed.error = submitted.error().toString();
        static_cast<void>(state.box->mailbox.post(generation, std::move(failed)));
    }
}

void WorldSession::beginRegeneration(ProductUi& product, ui::RegenerateKind kind, world::RegenerationScope scope,
                                     std::string_view via, const std::optional<ui::DirectoryCommand>& command) {
    const std::uint64_t frame = m_context.frameIndex();
    if (!m_regenerations || !m_regenerations->slot) {
        followRegeneration(product); // the state and the catalog's slot made
    }
    WorldRegenerations& state = *m_regenerations;
    if (!regenerationAllowed(product)) {
        spdlog::info("Frame {}: {} of {} refused for now (a floor on its way, a ride, a launch, the editor, another "
                     "regeneration)",
                     frame, ui::toString(kind), scope.toString());
        if (command) {
            product.refuseDirectory(*command, "change-unavailable");
        }
        return;
    }
    if (state.job == RegenerateJob::Prewarm) {
        // The prewarm's job is the slot's: the preview waits for it there (cancelled only once it ran: the slot keeps
        // its inputs whatever happens to the job's answer).
        state.job = RegenerateJob::None;
    }
    // The panel's name of the scope.
    ui::RegenerateScopeTitle title;
    if (kind == ui::RegenerateKind::Room) {
        title = roomTitleOf(scope.room, product.catalog());
    } else if (kind == ui::RegenerateKind::Floor) {
        title.floor = scope.floor;
    }
    product.openRegeneratePreview(kind, title);
    if (!product.regeneratePreviewOpen()) {
        spdlog::error("Frame {}: the preview cannot show; nothing is regenerated", frame);
        return;
    }
    m_interaction->bindNow(m_interaction->activeRoom()); // Focus left, as for a ride
    endRegeneration();
    state.request = RegenerateRequest{kind, scope, std::string(via)};
    state.panelState = ui::RegenerateState::Computing;
    state.requested = Clock::now();
    WorldRegenerationRecord record;
    record.kind = std::string(ui::toString(kind));
    record.scope = kind == ui::RegenerateKind::Undo ? std::string() : scope.toString();
    record.via = std::string(via);
    record.requestFrame = frame;
    m_record.regenerations.push_back(std::move(record));
    state.record = m_record.regenerations.size() - 1;
    spdlog::info("Frame {}: the preview of {} {} asked ({})", frame, ui::toString(kind),
                 kind == ui::RegenerateKind::Undo ? std::string() : scope.toString(), via);

    const WorldPaths paths = m_options.paths;
    const catalog::LibraryId library = m_planState->library;
    if (kind == ui::RegenerateKind::Undo) {
        submitRegenerateJob(RegenerateJob::Preview,
                            [paths, library, plan = m_planState->plan](const core::JobContext&) {
                                RegenerateJobAnswer answer;
                                answer.kind = RegenerateJobAnswer::Kind::Preview;
                                answer.undoPreview = previewUndo(paths, library, plan.get());
                                return answer;
                            });
        return;
    }
    // The plan shown: the preview takes it when it is its file's plan, its own digest the file's (m3/preview-time: not
    // parsed again). It may draw a stored building's lobby rooms on the hub (buildFloor()): then the file is read.
    const KnownPlan known{m_planState->plan, m_planState->digest};
    submitRegenerateJob(RegenerateJob::Preview, [paths, scope, library, known, slot = state.slot,
                                                 reader =
                                                     product.catalog().worldReader()](const core::JobContext& job) {
        RegenerateJobAnswer answer;
        answer.kind = RegenerateJobAnswer::Kind::Preview;
        // The world first, while the inputs the directory's tab reads ahead may still be on their way (m3/preview-time).
        RegenerationWorld world = readRegenerationWorld(paths, library, &known);
        const Clock::time_point start = Clock::now();
        const RegenerationInputsResult inputs = regenerationInputsFor(*slot, paths, reader.get(), answer.inputsTaken);
        answer.inputsMs = millisecondsSince(start);
        if (!inputs.inputs) {
            answer.failure = inputs.failure;
            answer.error = inputs.error;
            return answer;
        }
        answer.inputs = inputs.inputs;
        if (job.cancelled()) {
            return answer;
        }
        if (world.failure == regenerate_failure::kBusy) {
            // Read before the wait for the inputs: an action that was writing the world may have ended since.
            world = readRegenerationWorld(paths, library, &known);
        }
        answer.preview = previewRegeneration(world, scope, *inputs.inputs);
        return answer;
    });
}

void WorldSession::prewarmRegeneration(ProductUi& product) {
    WorldRegenerations& state = *m_regenerations;
    if (state.prewarmed || state.job != RegenerateJob::None || !m_planState || m_state != "room") {
        return;
    }
    const std::shared_ptr<WorldCatalogReader> reader = product.catalog().worldReader();
    if (!reader) {
        return;
    }
    state.prewarmed = true;
    spdlog::info("Frame {}: the regenerations' inputs are read ahead", m_context.frameIndex());
    submitRegenerateJob(RegenerateJob::Prewarm, [paths = m_options.paths, slot = state.slot, reader,
                                                 plan = m_planState->plan](const core::JobContext&) {
        RegenerateJobAnswer answer;
        bool taken = false;
        const RegenerationInputsResult inputs = regenerationInputsFor(*slot, paths, reader.get(), taken);
        answer.inputsTaken = taken;
        // Then the standing rooms' data (m3/preview-time), even once a preview was asked (its job's cancellation): a
        // preview reads the same through the inputs' loader, each room once, whichever reads it first. It stops once
        // the slot no longer holds these inputs (the directory closed and nothing needs them, a refresh of the
        // catalog, the session's end).
        if (inputs.inputs && plan) {
            static_cast<void>(warmRoomData(*inputs.inputs, *plan,
                                           [slot, held = inputs.inputs] { return slotHolds(*slot, held); }));
        }
        return answer;
    });
}

void WorldSession::followConfirmations(ProductUi& product) {
    WorldRegenerations& state = *m_regenerations;
    if (state.confirmationBuild != m_buildsRecorded) {
        // A new building's result (or a commit's floor): its own sentences, offered again even when they read as the
        // last ones did.
        state.confirmationBuild = m_buildsRecorded;
        state.confirmationRoom.reset();
        state.confirmationPending = false;
        if (!m_confirmationRooms.empty() && m_planState && m_planState->plan) {
            // ADR-0015 §3: the update left something as it stands because it would have moved what the user pinned;
            // the notice offers "Rigenera stanza" of the first room its sentences are about that stands.
            state.confirmationRoom = confirmationRoomOf(m_confirmationRooms, *m_planState->plan);
            if (state.confirmationRoom) {
                state.confirmationPending = true;
                spdlog::info("Frame {}: the notice will offer the preview of Rigenera stanza for {} ({} sentence(s) "
                             "of the update)",
                             m_context.frameIndex(), state.confirmationRoom->toString(), m_confirmationRooms.size());
            } else {
                spdlog::warn("Frame {}: the update's needsConfirmation names no room of the plan; no preview offered",
                             m_context.frameIndex());
            }
        }
    }
    // The build's own notice first (rooms migrated, a room kept...): the offer waits for it to hide.
    if (state.confirmationPending && !state.request && m_state == "room" && !noticeShowing()) {
        state.confirmationPending = false;
        const ui::RegenerateScopeTitle title = roomTitleOf(*state.confirmationRoom, product.catalog());
        setNotice(WorldNoticeContent{std::string(world_notice::kNeedsConfirmation), {}, noticeRoom(), title.roomKind,
                                     title.roomTitle},
                  true);
        // Its preview is not cold: the catalog's inputs read ahead now.
        prewarmRegeneration(product);
    }
}

void WorldSession::followRegeneration(ProductUi& product) {
    if (!m_regenerations) {
        m_regenerations = std::make_unique<WorldRegenerations>();
        m_regenerations->box = std::make_shared<RegenerateJobBox>();
        m_regenerations->box->wake = m_context.wakeCallback();
    }
    WorldRegenerations& state = *m_regenerations;
    // The catalog's inputs belong to its refresh: a refresh that ended makes them again.
    const std::string key = fmt::format("{}/{}", m_planState ? m_planState->library.str() : std::string(),
                                        m_lastRefresh.value_or(std::string()));
    if (!state.slot || key != state.slotKey) {
        if (state.slot) {
            state.slot->retired = true; // a read-ahead into the old inputs stops
        }
        state.slot = std::make_shared<RegenerationInputsSlot>();
        state.slotKey = key;
        state.prewarmed = false;
    }
    followConfirmations(product);
    // The job's answer (the latest asked: an earlier one's never takes its place).
    if (state.job != RegenerateJob::None) {
        if (std::optional<RegenerateJobAnswer> answer = state.box->mailbox.take(state.generation.current())) {
            if (state.job == RegenerateJob::Prewarm) {
                state.job = RegenerateJob::None;
            } else {
                takeRegenerateAnswer(product, std::move(*answer));
            }
        }
    }
    // A commit that never answers is given up: the panel says it failed (so that it closes, and regenerating() ends);
    // whatever it wrote, the floor follows the stored plan through a live update.
    if (state.job == RegenerateJob::Commit && Clock::now() - state.asked > kCommitPatience) {
        spdlog::error("Frame {}: the commit did not answer within {} s; given up (the floor follows the stored plan)",
                      m_context.frameIndex(), kCommitPatience.count());
        m_pool.cancel(state.generation);
        state.job = RegenerateJob::None;
        if (state.record && *state.record < m_record.regenerations.size()) {
            m_record.regenerations[*state.record].failure = std::string(regenerate_failure::kFailed);
            m_record.regenerations[*state.record].end = "failed";
        }
        if (state.request) {
            product.failRegeneratePreview(std::string(regenerate_failure::kFailed));
        }
        requestLiveUpdate();
    }
    // I4b step 6's review: the catalog's inputs (about 224 MiB of private bytes on the reference library) are let go of
    // while nothing needs them: the directory closed, no notice offering a preview, no preview, commit or undo asked or
    // running (a job that took them keeps them), no directory command waiting (a row that closed it, "Rigenera ...",
    // taken next frame). The tab or the notice reads them ahead again. The read-ahead itself needs nothing: while it
    // takes the inputs the slot is running (never let go of), and its rooms' data loop stops once they are let go of.
    const bool wanted = product.directoryOpen() || product.directoryCommandsPending() || confirmationOffered() ||
                        state.confirmationPending || state.request.has_value() ||
                        (state.job != RegenerateJob::None && state.job != RegenerateJob::Prewarm);
    if (releaseIdleInputs(*state.slot, wanted)) {
        state.prewarmed = false;
        ++m_record.regenerationInputsReleased;
        spdlog::info("Frame {}: the regenerations' inputs are let go of (nothing needs them now)",
                     m_context.frameIndex());
    }
    // The directory's "Modifiche" shown: the inputs read ahead.
    if (!state.request && product.directoryTab() == ui::DirectoryTab::Changes) {
        prewarmRegeneration(product);
    }
    if (!state.request) {
        return;
    }
    // The panel's choices.
    WorldRegenerationRecord* record = state.record && *state.record < m_record.regenerations.size()
                                          ? &m_record.regenerations[*state.record]
                                          : nullptr;
    const ui::RegenerateState panel = product.regeneratePreview().state();
    if (product.regeneratePreviewOpen() && panel != state.panelState) {
        if (panel == ui::RegenerateState::Confirm && record != nullptr) {
            ++record->confirmations;
        }
        state.panelState = panel;
    }
    followRegenerateChoices(product);
    // The panel committing with no commit under way (its preview lost): it says so, and can be closed.
    if (state.request && product.regeneratePreviewOpen() &&
        product.regeneratePreview().state() == ui::RegenerateState::Committing && state.job != RegenerateJob::Commit) {
        spdlog::error("Frame {}: the panel commits with no commit under way; it says the commit failed",
                      m_context.frameIndex());
        product.failRegeneratePreview(std::string(regenerate_failure::kFailed));
        if (state.record && *state.record < m_record.regenerations.size()) {
            m_record.regenerations[*state.record].failure = std::string(regenerate_failure::kFailed);
            m_record.regenerations[*state.record].end = "failed";
        }
    }
}

std::shared_ptr<const RegenerateShownFloor> WorldSession::shownFloorState() const {
    if (!m_shown || m_state != "room" || !m_planState) {
        return nullptr;
    }
    auto floor = std::make_shared<RegenerateShownFloor>();
    floor->shown = *m_shown;
    WorldBuild& frame = floor->frame;
    frame.library = m_planState->library;
    frame.startRoom = m_options.startRoom;
    frame.requestedFloor = m_floor;
    frame.outcome = WorldOutcome::Kept;
    frame.floor = m_shown->floor;
    frame.floorPlacement = m_shown->floorPlacement;
    frame.sceneDigest = m_shown->sceneDigest;
    frame.elevatorZone = m_elevatorZone;
    frame.elevatorDoors = m_elevatorDoors;
    frame.totemZone = m_totemZone;
    frame.totemScreen = m_totemScreen;
    frame.arrival = m_arrival;
    frame.showcase = m_showcaseSpec;
    frame.screenGames = m_shown->screenGames;
    if (m_shown->mediaKnown) {
        frame.mediaRoot = m_planState->mediaRoot;
    }
    frame.mediaRootPath = m_planState->mediaRootPath;
    frame.screenMediaRoot = m_planState->screenMediaRoot;
    for (const FloorRoom& room : m_rooms) {
        RegenerateShownFloor::Room shown;
        shown.doorway = room.doorway;
        if (const auto camera = m_doorways.find(room.id.toString()); camera != m_doorways.end()) {
            shown.doorwayCamera = camera->second;
        }
        if (m_planState->plan) {
            for (const world::Room& record : m_planState->plan->rooms) {
                if (record.id == room.id) {
                    shown.placement = record.placement;
                }
            }
        }
        floor->rooms.emplace(room.id, shown);
    }
    return floor;
}

void WorldSession::followRegenerateChoices(ProductUi& product) {
    WorldRegenerations& state = *m_regenerations;
    WorldRegenerationRecord* record = state.record && *state.record < m_record.regenerations.size()
                                          ? &m_record.regenerations[*state.record]
                                          : nullptr;
    for (const ui::RegenerateCommandKind command : product.takeRegenerateCommands()) {
        const std::uint64_t frame = m_context.frameIndex();
        if (record != nullptr) {
            record->choice = std::string(ui::toString(command));
            record->choiceFrame = frame;
        }
        if (command == ui::RegenerateCommandKind::Cancel) {
            spdlog::info("Frame {}: the preview of {} closed without a choice", frame,
                         ui::toString(state.request->kind));
            if (state.job == RegenerateJob::Preview) {
                m_pool.cancel(state.generation);
                state.job = RegenerateJob::None;
            }
            if (record != nullptr && record->end.empty()) {
                record->end = record->failure.empty() ? "cancelled" : "failed";
            }
            endRegeneration();
            return;
        }
        const auto failChoice = [&](std::string_view why) {
            spdlog::error("Frame {}: {} cannot be committed: {}", frame, ui::toString(command), why);
            product.failRegeneratePreview(std::string(regenerate_failure::kFailed));
            if (record != nullptr) {
                record->failure = std::string(regenerate_failure::kFailed);
                record->end = "failed";
            }
        };
        state.chosen = Clock::now();
        const WorldPaths paths = m_options.paths;
        const catalog::LibraryId library = m_planState->library;
        const std::string writer =
            m_options.writer.empty() ? std::string("retroverse (regeneration)") : m_options.writer + " (regeneration)";
        const std::shared_ptr<WorldCatalogReader> reader = product.catalog().worldReader();
        // The floor shown, built on the pool from the plan stored after the commit: the rooms it changed alone when
        // they can be, else the floor (as a ride's destination).
        FloorJob floorJob;
        floorJob.paths = paths;
        floorJob.skeleton.library = library;
        floorJob.skeleton.startRoom = m_options.startRoom;
        floorJob.skeleton.requestedFloor = m_floor;
        floorJob.skeleton.outcome = WorldOutcome::Kept;
        floorJob.sources.mediaRoot = m_planState->mediaRoot;
        floorJob.sources.mediaRootPath = m_planState->mediaRootPath;
        floorJob.sources.screenMediaRoot = m_planState->screenMediaRoot;
        floorJob.reader = reader;
        floorJob.shown = shownFloorState();
        floorJob.before = m_planState->plan;
        floorJob.sceneDigest = fmt::format("{}regenerate:{}", kEditSceneDigest, ++state.scenesRebuilt);
        if (command == ui::RegenerateCommandKind::Undo) {
            if (!state.undoPreview || !state.undoPreview->snapshot) {
                failChoice("no undo's preview");
                continue;
            }
            spdlog::info("Frame {}: Annulla ultima modifica: {} asked", frame, state.undoPreview->snapshot->name);
            submitRegenerateJob(RegenerateJob::Commit, [paths, library, writer, reader, slot = state.slot,
                                                        expected = state.undoPreview->snapshot->name,
                                                        floorJob = std::move(floorJob)](const core::JobContext&) {
                RegenerateJobAnswer answer;
                answer.kind = RegenerateJobAnswer::Kind::Commit;
                // The catalog's inputs (read ahead, taken or waited for: I4b step 6's review), so that the items
                // files are always written again from the catalog's snapshot.
                bool taken = false;
                const RegenerationInputsResult inputs = regenerationInputsFor(*slot, paths, reader.get(), taken);
                answer.inputsTaken = taken;
                if (!inputs.inputs) {
                    spdlog::warn("Undo: the catalog's inputs cannot be read ({}); the items files are not written "
                                 "again",
                                 inputs.error);
                }
                const BuildingSnapshot* snapshot = inputs.inputs ? inputs.inputs->snapshot.get() : nullptr;
                // The plan shown is the one the undo replaces (only the rooms that change get their items again).
                UndoCommit undone = commitUndo(paths, library, expected, writer, snapshot, floorJob.before.get());
                if (undone.failure.empty() && undone.plan) {
                    buildCommittedFloor(floorJob, undone.plan, undone.planDigest, undone.items, answer);
                }
                answer.undone = std::move(undone);
                return answer;
            });
            continue;
        }
        if (!state.preview || !state.inputs) {
            failChoice("no preview");
            continue;
        }
        const bool drop = command == ui::RegenerateCommandKind::RegenerateDrop;
        spdlog::info("Frame {}: {} of {} asked{}", frame, drop ? "Rigenera e scarta le personalizzazioni" : "Rigenera",
                     state.preview->scope.toString(), drop ? " (the second confirmation given)" : "");
        submitRegenerateJob(RegenerateJob::Commit, [paths, writer, drop, preview = *state.preview,
                                                    inputs = state.inputs,
                                                    floorJob = std::move(floorJob)](const core::JobContext&) {
            RegenerateJobAnswer answer;
            answer.kind = RegenerateJobAnswer::Kind::Commit;
            RegenerationCommit commit = commitRegeneration(paths, preview, drop, *inputs, writer);
            if (commit.failure.empty() && commit.changed && commit.plan) {
                buildCommittedFloor(floorJob, commit.plan, commit.planDigest, commit.items, answer);
            }
            answer.commit = std::move(commit);
            return answer;
        });
    }
}

void WorldSession::endRegeneration() {
    if (!m_regenerations) {
        return;
    }
    WorldRegenerations& state = *m_regenerations;
    // The panel's request and what it held (a preview's plans and regeneration, the inputs' reference); the slot keeps
    // the inputs until the catalog's refresh.
    state.request.reset();
    state.preview.reset();
    state.inputs.reset();
    state.undoPreview.reset();
}

void WorldSession::adoptPlan(const std::shared_ptr<const world::WorldPlan>& plan, const std::string& digest) {
    if (!plan || !m_planState || m_planState->digest == digest) {
        return;
    }
    // What keepPlan() and keepShown() derive from a plan shown: the rides' floors, where-is, the card's versions,
    // Informazioni, the floors' record (the directory follows the digest).
    m_planState->plan = plan;
    m_planState->digest = digest;
    m_planIndex.emplace(*plan);
    m_record.floors = floorRecordsOf(*plan);
    m_otherVersions = otherVersionCounts(*plan);
    m_launch.setPlan(plan.get());
    m_versionGroups = VersionGroups::of(*plan);
}

bool WorldSession::showRoomsAlone(WorldBuild& build, std::span<const RoomStaticPart> statics,
                                  WorldLiveUpdateRecord& record, bool& replaced) {
    replaced = false;
    if (!m_shown || !build.plan) {
        return false;
    }
    // As the editor shows a room rebuilt alone (showEditBuild()): planned as the floor will be once the parts are
    // replaced (its scene digest the build's), before anything changes.
    const std::string shownDigest = std::exchange(m_shown->sceneDigest, build.sceneDigest);
    LiveUpdatePlan plan = planLiveUpdate(*m_shown, build, true);
    m_shown->sceneDigest = shownDigest;
    if (plan.kind == LiveUpdateKind::Rebuild || plan.kind == LiveUpdateKind::Kept ||
        (!statics.empty() && !m_host.replaceObjects)) {
        spdlog::warn("World: the rooms rebuilt alone cannot be shown in place ({})",
                     m_host.replaceObjects ? plan.reason : std::string("no scene to replace them in"));
        return false;
    }
    if (!statics.empty()) {
        // The rebuilt rooms' collections applied again even when their items stand as they did: their occluders and
        // their fixtures' pick targets moved with the furniture.
        plan.kind = LiveUpdateKind::Items;
        plan.reloadCovers = true;
        for (RoomUpdate& update : plan.rooms) {
            const bool rebuilt = std::any_of(statics.begin(), statics.end(),
                                             [&](const RoomStaticPart& part) { return part.room == update.room; });
            if (rebuilt && (update.kind == RoomUpdateKind::Unchanged || update.kind == RoomUpdateKind::Textures)) {
                update.kind = RoomUpdateKind::Items;
            }
        }
    }
    for (const RoomStaticPart& part : statics) {
        SceneObjectsChange change;
        change.prefix = part.room.toString() + "/";
        change.objects = part.objects;
        change.lights = part.lights;
        if (!m_host.replaceObjects(change)) {
            spdlog::error("World: the static part of {} cannot be replaced in the scene", part.room.toString());
            return false;
        }
        replaced = true;
    }
    record.rooms = roomRecords(plan, false);
    if (!applyRooms(build, plan, record)) {
        return false;
    }
    // The floor shown's static part is the build's from now on (keepShown() took the build).
    m_shown->sceneDigest = build.sceneDigest;
    return true;
}

void WorldSession::takeRegenerateAnswer(ProductUi& product, RegenerateJobAnswer answer) {
    WorldRegenerations& state = *m_regenerations;
    state.job = RegenerateJob::None;
    const std::uint64_t frame = m_context.frameIndex();
    WorldRegenerationRecord* record = state.record && *state.record < m_record.regenerations.size()
                                          ? &m_record.regenerations[*state.record]
                                          : nullptr;
    const auto fail = [&](const std::string& failure, const std::string& error) {
        spdlog::warn("Frame {}: {} failed ({}): {}", frame,
                     state.request ? ui::toString(state.request->kind) : std::string_view("the regeneration"), failure,
                     error);
        if (state.request) {
            product.failRegeneratePreview(failure);
        }
        if (record != nullptr) {
            record->failure = failure;
            record->end = "failed";
        }
    };
    if (answer.kind == RegenerateJobAnswer::Kind::Preview) {
        if (!state.request) {
            return; // the panel closed meanwhile
        }
        if (record != nullptr) {
            record->previewFrame = frame;
            record->previewMs = millisecondsSince(state.requested);
            record->previewJobMs = answer.milliseconds;
            record->inputsTaken = answer.inputsTaken;
            if (answer.inputsTaken) {
                record->inputsMs = answer.inputsMs;
            }
        }
        if (!answer.failure.empty()) {
            fail(answer.failure, answer.error);
            return;
        }
        if (answer.undoPreview) {
            UndoPreview& undo = *answer.undoPreview;
            if (!undo.failure.empty()) {
                fail(undo.failure, undo.error);
                return;
            }
            // The undone room named as the panel names rooms.
            if (const auto room = world::RoomId::parse(undo.content.undoScope)) {
                undo.content.undoScopeTitle = roomTitleOf(*room, product.catalog());
            }
            if (record != nullptr) {
                record->undoSnapshot = undo.snapshot ? undo.snapshot->name : std::string();
                record->scope = undo.content.undoScope;
            }
            product.setRegeneratePreview(undo.content);
            state.undoPreview = std::move(undo);
            spdlog::info("Frame {}: the preview of Annulla ultima modifica shown ({:.0f} ms)", frame,
                         record != nullptr ? record->previewMs.value_or(0.0) : 0.0);
            return;
        }
        if (!answer.preview) {
            fail(std::string(regenerate_failure::kFailed), "no preview");
            return;
        }
        RegenerationPreview& preview = *answer.preview;
        if (!preview.failure.empty()) {
            fail(preview.failure, preview.error);
            return;
        }
        if (record != nullptr) {
            copyCounts(preview.content, *record);
            record->regenerateMs = preview.regenerateMs;
        }
        product.setRegeneratePreview(preview.content);
        state.inputs = answer.inputs;
        state.preview = std::move(preview);
        spdlog::info("Frame {}: the preview of {} shown ({:.0f} ms after the request; the job {:.0f} ms: the world "
                     "read {:.0f} ms{}, its inputs {}, the dry run {:.0f} ms)",
                     frame, state.preview->scope.toString(), record != nullptr ? record->previewMs.value_or(0.0) : 0.0,
                     answer.milliseconds, state.preview->readMs, state.preview->knownPlan ? " (the plan shown)" : "",
                     answer.inputsTaken      ? fmt::format("taken in {:.0f} ms", answer.inputsMs)
                     : answer.inputsMs >= 1.0 ? fmt::format("waited for {:.0f} ms (read ahead)", answer.inputsMs)
                                              : std::string("ready"),
                     state.preview->totalMs - state.preview->readMs);
        return;
    }

    // A commit's answer: taken whatever the panel shows (the floor follows the plan stored).
    if (record != nullptr) {
        record->commitMs = answer.milliseconds;
    }
    std::string failure = answer.failure;
    std::string error = answer.error;
    std::shared_ptr<const world::WorldPlan> plan;
    std::string digest;
    std::string scopeText;
    bool written = false;
    bool changed = false;
    std::map<catalog::SystemName, RoomItems> items;
    if (answer.commit) {
        failure = answer.commit->failure;
        error = answer.commit->error;
        plan = answer.commit->plan;
        digest = answer.commit->planDigest;
        written = answer.commit->written;
        changed = answer.commit->changed;
        scopeText = answer.commit->failure.empty() && state.preview ? state.preview->scope.toString() : std::string();
        if (record != nullptr) {
            record->written = written;
            record->reused = answer.commit->reused;
            record->snapshot = answer.commit->snapshot;
        }
    } else if (answer.undone) {
        failure = answer.undone->failure;
        error = answer.undone->error;
        plan = answer.undone->plan;
        digest = answer.undone->planDigest;
        written = answer.undone->written;
        changed = written;
        scopeText = state.undoPreview ? state.undoPreview->content.undoScope : std::string();
        if (record != nullptr) {
            record->written = written;
            record->snapshot = answer.undone->kept;
        }
    } else if (!failure.empty()) {
        // The job stopped before its commit answered (an exception): what it wrote, the live update shows.
        requestLiveUpdate();
    }
    if (!failure.empty()) {
        fail(failure, error);
        return;
    }
    if (record != nullptr) {
        record->planDigest = digest;
    }
    m_record.planDigest = digest.empty() ? m_record.planDigest : digest;
    m_record.planWritten = m_record.planWritten || written;
    // The panel closes; the floor shown follows the plan stored now.
    product.closeRegeneratePreview();
    std::string path = "none";
    std::string walker = "kept";
    bool shown = false;
    if (changed && answer.build && m_shown && m_state == "room") {
        const std::shared_ptr<const world::WorldPlan> before = m_planState ? m_planState->plan : nullptr;
        const std::string area = m_host.eyeArea ? m_host.eyeArea() : std::string();
        WorldBuild& build = *answer.build;
        // The commit's times as the job's record (the run report's world.job: the plan shown was made by it).
        build.written = written;
        build.milliseconds = answer.milliseconds;
        build.floorMs = answer.floorMs;
        if (answer.commit) {
            build.lockMs = answer.commit->lockMs;
            build.planMs = answer.commit->regenerateMs;
            build.saveMs = answer.commit->commitMs;
            build.itemsMs = answer.commit->itemsMs;
        } else if (answer.undone) {
            build.lockMs = answer.undone->lockMs;
            build.saveMs = answer.undone->undoMs;
            build.itemsMs = answer.undone->itemsMs;
        }
        recordBuild(build);
        WorldLiveUpdateRecord live;
        releaseHero();
        if (answer.alone) {
            // I4b step 6's review: the rooms the commit changed rebuilt alone in place (step 4's path).
            bool replaced = false;
            shown = showRoomsAlone(build, answer.statics, live, replaced);
            path = shown ? "room" : "none";
            if (shown) {
                wantShowcase();
            } else if (answer.fallback && plan) {
                // Their parts cannot be shown in place: the floor of the plan stored, built here and committed (the
                // scene never keeps a part the floor does not hold).
                spdlog::warn("Frame {}: the rooms rebuilt alone cannot be shown{}; the floor is built here", frame,
                             replaced ? " (some parts were replaced)" : std::string());
                WorldBuild floor;
                floor.library = build.library;
                floor.startRoom = m_options.startRoom;
                floor.requestedFloor = m_floor;
                floor.outcome = WorldOutcome::Kept;
                floor.plan = *plan;
                floor.planDigest = digest;
                floor.reason = "a regeneration's floor";
                buildFloor(m_options.paths, floor, *answer.fallback);
                if (floor.plan && floor.scene) {
                    shown = showFloor(floor, true, &live.walker);
                    path = shown ? "floor" : "none";
                }
            }
        } else {
            const LiveUpdatePlan update = planLiveUpdate(*m_shown, build);
            if (update.kind == LiveUpdateKind::Unchanged || update.kind == LiveUpdateKind::Textures ||
                update.kind == LiveUpdateKind::Items) {
                live.rooms = roomRecords(update, false);
                shown = applyRooms(build, update, live);
                path = "in-place";
                if (shown) {
                    wantShowcase(); // as applyLive(): a floor kept makes the hub's showcase again
                }
            }
            if (!shown) {
                // The floor's static part changed (a room's fixtures, a room moved): committed again, the walker kept
                // in the frame of the area that holds it (ADR-0013 §7).
                shown = showFloor(build, true, &live.walker);
                path = shown ? "floor" : "none";
            }
        }
        if (!shown) {
            spdlog::error("Frame {}: the floor after the commit cannot be shown; the floor follows the stored plan "
                          "through a live update",
                          frame);
        } else if (regenerationCovers(area, scopeText, before.get())) {
            // ADR-0015 §5: a walker in a regenerated room stands at its doorway, facing in (the elevator when the room
            // is gone from the floor, or when it stood in the hall of a floor whose rooms were placed again).
            const bool standing = m_doorways.contains(area);
            const scene::CameraSetup& camera = standing ? m_doorways.at(area) : m_arrival;
            if (m_host.teleport) {
                m_host.teleport(camera);
            }
            m_interaction->bindNow(standing ? interactionRoomOf(area) : std::nullopt);
            TeleportRecord teleport;
            teleport.frame = frame;
            teleport.to = standing ? area : std::string("elevator");
            teleport.reason = "regeneration";
            if (m_host.walker) {
                teleport.walker = spawnOf(m_host.walker());
            }
            m_record.teleports.push_back(std::move(teleport));
            walker = standing ? "doorway:" + area : std::string("elevator");
            spdlog::info("Frame {}: the walker stands at {} (its {} was regenerated)", frame,
                         standing ? area + "'s doorway" : std::string("the elevator"),
                         standing || world::RoomId::parse(area) ? "room" : "floor");
        }
        m_record.state = m_state;
    } else if (changed && !answer.buildError.empty()) {
        spdlog::error("Frame {}: the floor after the commit: {}", frame, answer.buildError);
    }
    if (changed && !shown) {
        // The view follows the stored plan whatever could be shown: the plan's derived state now, the floor through a
        // live update (also a plan restored that could not be read back).
        adoptPlan(plan, digest);
        requestLiveUpdate();
    } else if (!changed) {
        adoptPlan(plan, digest); // nothing to write: the plan stored as read under the lock
    }
    if (record != nullptr) {
        record->path = path;
        record->walker = walker;
        record->backMs = millisecondsSince(state.chosen);
        record->end = "committed";
    }
    state.backPending = true;
    // The notice's offer is answered (the next build that needs a confirmation offers it again).
    state.confirmationRoom.reset();
    state.confirmationPending = false;
    if (m_notice.notice == world_notice::kNeedsConfirmation) {
        setNotice(WorldNoticeContent{}, false);
    }
    spdlog::info("Frame {}: {} committed{} ({}; {:.0f} ms after the choice)", frame,
                 state.request ? ui::toString(state.request->kind) : std::string_view("the regeneration"),
                 changed ? std::string(" and shown") : std::string(": nothing to change"), path,
                 millisecondsSince(state.chosen));
    endRegeneration();
}

void WorldSession::regenerationFrameRendered() {
    if (!m_regenerations || !std::exchange(m_regenerations->backPending, false) || !m_regenerations->record) {
        return;
    }
    if (*m_regenerations->record < m_record.regenerations.size()) {
        WorldRegenerationRecord& record = m_record.regenerations[*m_regenerations->record];
        record.backFrame = m_context.frameIndex();
        record.backMs = millisecondsSince(m_regenerations->chosen);
    }
}

} // namespace rv::app
