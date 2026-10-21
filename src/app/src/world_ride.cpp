// The elevator, the directory and the ride of the building in the app (I4 step 7b; ADR-0013 §10, §13): the part of
// app::WorldSession that takes the directory's commands and "Mostra nella stanza" where they lead (directory_data.hpp's
// routing), teleports the walker on the floor shown, and rides to another floor (elevator_ride.hpp's machine, a
// floor-build job from the plan shown, the floor swapped behind the fade).

#include <algorithm>
#include <mutex>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/app/frame_timing.hpp>
#include <rv/app/world_launch_ui.hpp>
#include <rv/render/gpu_memory.hpp>

#include "cover_loader.hpp"
#include "product_ui.hpp"
#include "world_session.hpp"

namespace rv::app {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

} // namespace

float WorldSession::fade() const noexcept {
    return m_ride ? m_ride->fade() : 0.0f;
}

bool WorldSession::atElevator() const {
    if (m_state != "room" || m_floor.empty() || !m_host.walker || m_elevatorZone.isEmpty()) {
        return false;
    }
    const glm::vec3 feet = m_host.walker().feet;
    return feet.x >= m_elevatorZone.min.x && feet.x <= m_elevatorZone.max.x && feet.z >= m_elevatorZone.min.z &&
           feet.z <= m_elevatorZone.max.z;
}

bool WorldSession::idleAtElevator() const {
    const WorldInteraction& interaction = m_interaction->interaction();
    return !m_ride && interaction.state() == WorldState::FreeRoam && interaction.hovered() == world::kNoPick &&
           atElevator();
}

void WorldSession::openDirectory(ProductUi& product, ui::DirectoryTab tab, std::string_view via) {
    syncDirectory(product);
    product.openDirectory(tab, via);
}

void WorldSession::elevatorPrompt(ProductUi& product, std::optional<std::string> key) {
    if (key) {
        ++m_record.elevatorPromptFrames;
    }
    product.setElevatorPrompt(std::move(key));
}

WorldMoveConditions WorldSession::moveConditions(const ProductUi& product) const {
    WorldMoveConditions conditions;
    conditions.planReady = m_state == "room" && m_planState.has_value() && m_planIndex.has_value() && !m_floor.empty();
    conditions.applying = m_shownPreview.has_value() || m_previewAwaitingFrame || m_retargetPending;
    conditions.riding = m_ride != nullptr;
    conditions.launchActive = product.launchActive();
    conditions.state = m_interaction->interaction().state();
    return conditions;
}

std::optional<std::size_t> WorldSession::interactionRoomOf(std::string_view room) const {
    for (const FloorRoom& floorRoom : m_rooms) {
        if (floorRoom.id.toString() == room) {
            return floorRoom.interactionRoom;
        }
    }
    return std::nullopt;
}

void WorldSession::keepPlan(const WorldBuild& build) {
    m_floor = build.floor;
    m_elevatorZone = build.elevatorZone;
    m_arrival = build.arrival;
    m_doorways.clear();
    for (const RoomCollection& room : build.rooms) {
        m_doorways[room.room.toString()] = room.doorwayCamera;
    }
    if (!build.plan) {
        return;
    }
    if (!m_planState || m_planState->digest != build.planDigest) {
        PlanState state;
        state.plan = std::make_shared<const world::WorldPlan>(*build.plan);
        state.digest = build.planDigest;
        m_planState = std::move(state);
        m_planIndex.emplace(*build.plan);
        m_record.floors = floorRecordsOf(*build.plan);
    }
    // The media roots as this build knows them (the preview's are unknown until the building's result).
    m_planState->library = build.library;
    m_planState->mediaRoot = build.mediaRoot;
    m_planState->mediaRootPath = build.mediaRootPath;
    m_planState->screenMediaRoot = build.screenMediaRoot;
}

void WorldSession::syncDirectory(ProductUi& product) {
    if (m_planState) {
        const std::string key = fmt::format("{}/{}", m_planState->digest, product.catalogRevision());
        if (key != m_directoryPlan) {
            AppCatalog& catalog = product.catalog();
            m_directoryBuilding = directoryBuildingOf(*m_planState->plan, [&catalog](const catalog::SystemName& system) {
                return catalog.systemFullName(system);
            });
            m_directoryPlan = key;
        }
    }
    ui::DirectoryBuilding building = m_directoryBuilding;
    building.currentFloor = m_state == "room" ? m_floor : std::string();
    building.currentRoom.clear();
    if (const std::optional<std::size_t> eye = eyeRoom(); eye && m_state == "room") {
        building.currentRoom = m_rooms[*eye].id.toString();
    }
    product.setDirectoryBuilding(std::move(building));
}

void WorldSession::followCommands(ProductUi& product) {
    const std::uint64_t frame = m_context.frameIndex();
    const std::optional<world::RoomId> active = m_interaction->activeRoomId();
    const std::string eyeRoomId = active ? active->toString() : std::string();
    const world::WorldPlan* plan = m_planState ? m_planState->plan.get() : nullptr;
    const world::PlanIndex* index = m_planIndex ? &*m_planIndex : nullptr;
    for (const ui::DirectoryCommand& command : product.takeDirectoryCommands()) {
        const WorldMove move =
            routeDirectoryCommand(command, moveConditions(product), plan, index, m_floor, eyeRoomId);
        spdlog::info("Frame {}: the directory's {} '{}': {} ({})", frame, ui::toString(command.kind), command.id,
                     toString(move.kind), move.why);
        std::optional<catalog::GameId> game;
        if (command.kind == ui::DirectoryCommandKind::ShowInRoom) {
            if (const auto parsed = catalog::GameId::parse(command.id)) {
                game = *parsed;
            }
        }
        executeMove(product, move, command, game);
    }
    // "Mostra nella stanza" of the quick catalog's card, pressed in the catalog that closed at the end of the last frame.
    if (const std::optional<std::string> asked = product.takeShowInRoom()) {
        if (const auto game = catalog::GameId::parse(*asked)) {
            const WorldMove move = routeShowInRoom(*game, moveConditions(product), index, m_floor, eyeRoomId);
            spdlog::info("Frame {}: Mostra nella stanza on {}: {} ({})", frame, game->toString(), toString(move.kind),
                         move.why);
            executeMove(product, move, std::nullopt, std::optional<catalog::GameId>(*game));
        } else {
            spdlog::warn("Frame {}: Mostra nella stanza on '{}': not a GameId", frame, *asked);
        }
    }
    // After a ride: the game shown in its room once the walker stands at its doorway and the view is back.
    if (m_pendingShow && !m_ride) {
        const PendingShow show = *std::exchange(m_pendingShow, std::nullopt);
        m_interaction->bindNow(interactionRoomOf(show.room));
        static_cast<void>(m_launch.showInRoom(show.game, *m_interaction, m_host.walker(), frame));
    }
}

void WorldSession::executeMove(ProductUi& product, const WorldMove& move,
                               const std::optional<ui::DirectoryCommand>& command,
                               const std::optional<catalog::GameId>& game) {
    const std::uint64_t frame = m_context.frameIndex();
    switch (move.kind) {
    case WorldMoveKind::Ignore:
        if (game) {
            m_launch.showInRoomRefused(*game, ui::quick_catalog::kRoomNotInRoom, frame);
        }
        break;
    case WorldMoveKind::Refuse:
        if (command) {
            product.refuseDirectory(*command, move.notice);
        }
        if (game) {
            m_launch.showInRoomRefused(*game, move.notice, frame);
        }
        break;
    case WorldMoveKind::ShowHere:
        if (game) {
            static_cast<void>(m_launch.showInRoom(*game, *m_interaction, m_host.walker(), frame));
        }
        break;
    case WorldMoveKind::Teleport:
        teleportTo(move.room, move.reason);
        if (game) {
            // From the doorway, eased to the item's Focus pose (ADR-0013 §10: "Same floor").
            static_cast<void>(m_launch.showInRoom(*game, *m_interaction, m_host.walker(), frame));
        }
        break;
    case WorldMoveKind::Ride:
        startRide(move, command);
        break;
    }
}

void WorldSession::teleportTo(const std::string& room, RideReason reason) {
    const scene::CameraSetup* camera = &m_arrival;
    if (!room.empty()) {
        const auto found = m_doorways.find(room);
        if (found == m_doorways.end()) {
            spdlog::warn("Frame {}: no doorway of {} on the floor shown: the walker stays", m_context.frameIndex(),
                         room);
            return;
        }
        camera = &found->second;
    }
    if (m_host.teleport) {
        m_host.teleport(*camera);
    }
    // The interaction in the room the walker stands in now (Focus left first).
    m_interaction->bindNow(room.empty() ? std::nullopt : interactionRoomOf(room));
    TeleportRecord record;
    record.frame = m_context.frameIndex();
    record.to = room.empty() ? std::string("elevator") : room;
    record.reason = std::string(toString(reason));
    if (m_host.walker) {
        record.walker = spawnOf(m_host.walker());
    }
    spdlog::info("Frame {}: the walker teleported to {} on {} ({})", record.frame, record.to, m_floor, record.reason);
    m_record.teleports.push_back(std::move(record));
}

void WorldSession::startRide(const WorldMove& move, const std::optional<ui::DirectoryCommand>& command) {
    const std::uint64_t frame = m_context.frameIndex();
    RideRequest request;
    request.from = m_floor;
    request.to = move.floor;
    request.reason = move.reason;
    request.room = move.room;
    request.game = move.game;
    RideSettings settings;
    settings.reduceMotion = m_reduceMotion;
    settings.lockstep = m_options.lockstep;
    m_ride = std::make_unique<ElevatorRide>(std::move(request), settings, frame);
    m_rideStart = Clock::now();
    m_rideCommand = command;
    m_rideBuild.reset();
    m_pendingShow.reset();
    // ADR-0013 §10: "Mostra nella stanza" leaves Focus first (the ride takes no other state: moveConditions()).
    m_interaction->bindNow(m_interaction->activeRoom());
    m_context.frameTiming().openPhase(kPhaseElevator);
    WorldBuild skeleton;
    skeleton.library = m_planState->library;
    skeleton.startRoom = m_options.startRoom;
    skeleton.requestedFloor = move.floor;
    skeleton.outcome = WorldOutcome::Kept;
    skeleton.planDigest = m_planState->digest;
    skeleton.reason = "a ride's destination";
    requestFloor(std::move(skeleton), false);
    spdlog::info("Frame {}: the elevator rides from {} to {} ({}{}{})", frame, m_ride->request().from, move.floor,
                 toString(move.reason), move.room.empty() ? "" : ", ", move.room);
}

void WorldSession::requestFloor(WorldBuild skeleton, bool retarget) {
    // At most one floor-build job: the latest asked wins.
    m_pool.cancel(m_floorGeneration);
    const std::uint64_t generation = m_floorGeneration.current();
    {
        const std::lock_guard lock(m_floorBox->mutex);
        m_floorBox->build.reset();
    }
    FloorSources sources;
    sources.mediaRoot = m_planState->mediaRoot;
    sources.mediaRootPath = m_planState->mediaRootPath;
    sources.screenMediaRoot = m_planState->screenMediaRoot;
    core::JobOptions options;
    options.priority = core::WorkerPool::kHighestPriority;
    options.generation = &m_floorGeneration;
    auto shared = std::make_shared<WorldBuild>(std::move(skeleton));
    const Status submitted = m_pool.submit(
        [box = m_floorBox, paths = m_options.paths, plan = m_planState->plan, sources = std::move(sources), shared,
         generation, retarget](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            WorldBuild build = std::move(*shared);
            if (!build.plan) {
                build.plan = *plan;
            }
            buildFloor(paths, build, sources);
            if (job.cancelled()) {
                return;
            }
            std::function<void()> wake;
            {
                const std::lock_guard lock(box->mutex);
                box->build = std::move(build);
                box->generation = generation;
                box->retarget = retarget;
                wake = box->wake;
            }
            if (wake) {
                wake();
            }
        },
        options);
    m_retargetPending = retarget && submitted;
    if (!submitted) {
        spdlog::error("World: the floor's job cannot be submitted: {}", submitted.error().toString());
        if (m_ride && !retarget) {
            m_ride->floorAnswered(false, millisecondsSince(m_rideStart), std::nullopt);
        }
    }
}

std::optional<WorldBuild> WorldSession::takeFloorBuild(bool& retarget) {
    const std::lock_guard lock(m_floorBox->mutex);
    if (!m_floorBox->build || m_floorBox->generation != m_floorGeneration.current()) {
        return std::nullopt;
    }
    retarget = m_floorBox->retarget;
    return std::exchange(m_floorBox->build, std::nullopt);
}

bool WorldSession::retargetResult(WorldResult& result) {
    if (result.testRoom || !result.build || !result.build->plan || result.build->startRoomMissing ||
        m_floor.empty() || result.build->floor.empty() || result.build->floor == m_floor || m_state != "room") {
        return false;
    }
    spdlog::info("Frame {}: the building's result was built for {}, the walker is on {} since: built again for it",
                 m_context.frameIndex(), result.build->floor, m_floor);
    WorldBuild skeleton = std::move(*result.build);
    // Its floor's part goes; buildFloor() makes the floor shown's from its plan.
    skeleton.scene.reset();
    skeleton.sceneDigest.clear();
    skeleton.undressed.clear();
    skeleton.rooms.clear();
    skeleton.screenGames.clear();
    skeleton.objectFallbacks.clear();
    skeleton.itemsRead = 0;
    skeleton.floor.clear();
    skeleton.requestedFloor = m_floor;
    // The plan the job reads its media roots with: this result's.
    PlanState state;
    state.plan = std::make_shared<const world::WorldPlan>(*skeleton.plan);
    state.digest = skeleton.planDigest;
    state.library = skeleton.library;
    state.mediaRoot = skeleton.mediaRoot;
    state.mediaRootPath = skeleton.mediaRootPath;
    state.screenMediaRoot = skeleton.screenMediaRoot;
    m_planState = std::move(state);
    m_planIndex.emplace(*skeleton.plan);
    m_record.floors = floorRecordsOf(*skeleton.plan);
    requestFloor(std::move(skeleton), true);
    return true;
}

bool WorldSession::rideViewResident() const {
    const std::optional<std::size_t> eye = eyeRoom();
    if (!eye || !m_loader) {
        return true; // a hall (no item), or a floor without covers to wait for
    }
    return m_loader->groupDone(static_cast<std::uint32_t>(*eye));
}

void WorldSession::followRide(ProductUi& product, bool idleWake, double deltaSeconds) {
    // The floor-build job's answer: a ride's destination, or a building's result built again for the floor shown.
    bool retarget = false;
    if (std::optional<WorldBuild> built = takeFloorBuild(retarget)) {
        if (retarget) {
            m_retargetPending = false;
            WorldResult result;
            result.notice = built->notice;
            result.reason = built->reason;
            result.build = std::move(*built);
            applyLive(std::move(result));
        } else if (m_ride) {
            const bool ok = built->plan.has_value() && built->scene.has_value() && !built->startRoomMissing;
            if (!ok) {
                spdlog::error("World: the floor {} cannot be built for the ride: {}", m_ride->request().to,
                              built->reason);
            }
            m_ride->floorAnswered(ok, millisecondsSince(m_rideStart), built->floorMs);
            if (ok) {
                m_rideBuild = std::move(*built);
            }
        }
    }
    if (!m_ride) {
        return;
    }
    // An idle wake (a lockstep wait) only takes the destination once it is built: the fades and the view's wait move
    // with frames, so that a scripted run's frames are the same every time.
    if (idleWake && m_ride->phase() != RidePhase::AwaitingFloor) {
        return;
    }
    RideFrame frame;
    frame.frame = m_context.frameIndex();
    frame.deltaSeconds = idleWake ? 0.0 : deltaSeconds;
    frame.elapsedMs = millisecondsSince(m_rideStart);
    frame.viewResident = !idleWake && m_ride->hasSwapped() && m_updates > m_swapUpdate && rideViewResident();
    switch (m_ride->update(frame)) {
    case RideAction::None:
        break;
    case RideAction::Swap:
        swapRide();
        m_ride->swapped(m_context.frameIndex());
        m_swapUpdate = m_updates;
        break;
    case RideAction::Fail:
        // The walker stays where it was; the directory says so when it asked.
        if (m_rideCommand) {
            product.refuseDirectory(*m_rideCommand, kNoticeRideUnavailable);
        }
        m_rideBuild.reset();
        break;
    case RideAction::Finish:
        finishRide();
        break;
    }
}

void WorldSession::swapRide() {
    WorldBuild build = std::move(*m_rideBuild);
    m_rideBuild.reset();
    const RideRequest& request = m_ride->request();
    // The walker at the destination's elevator, facing the hall, or at the room's doorway (ADR-0013 §10).
    scene::CameraSetup at = build.arrival;
    if (!request.room.empty()) {
        if (const auto room = world::RoomId::parse(request.room)) {
            if (const RoomCollection* collection = build.findRoom(*room)) {
                at = collection->doorwayCamera;
            }
        }
    }
    build.scene->camera.position = at.position;
    build.scene->camera.yawDeg = at.yawDeg;
    build.scene->camera.pitchDeg = at.pitchDeg;
    const Clock::time_point start = Clock::now();
    const std::string from = m_floor;
    if (!showFloor(build, false, nullptr, true)) {
        spdlog::error("World: the ride's floor {} could not be shown", request.to);
        return;
    }
    m_record.state = m_state;
    spdlog::info("Frame {}: the ride swapped {} for {} in {:.1f} ms ({} item(s) in {} room(s))",
                 m_context.frameIndex(), from, build.floor, millisecondsSince(start), m_record.items,
                 build.rooms.size());
}

void WorldSession::finishRide() {
    RideRecord record = m_ride->record();
    if (m_host.walker) {
        record.walker = spawnOf(m_host.walker());
    }
    const GpuMemoryRecord memory = gpuMemoryRecord(gl::gpuMemoryLedger());
    record.arrayBytes = memory.usage(gl::GpuMemoryKind::CollectionArrays).bytes;
    record.gpuBytes = memory.totalBytes;
    m_context.frameTiming().closePhase(kPhaseElevator);
    spdlog::info("Frame {}: the ride from {} to {} {}: the view back {:.0f} ms after it was asked (the floor built at "
                 "{:.0f} ms, its job {:.0f} ms){}; GPU memory {:.1f} MiB, arrays {:.1f} MiB",
                 m_context.frameIndex(), record.from, record.to, record.outcome, record.totalMs.value_or(0.0),
                 record.builtMs.value_or(0.0), record.jobMs.value_or(0.0),
                 record.residentMs ? fmt::format(", the eye's area resident at {:.0f} ms", *record.residentMs)
                                   : std::string(),
                 static_cast<double>(record.gpuBytes) / (1024.0 * 1024.0),
                 static_cast<double>(record.arrayBytes) / (1024.0 * 1024.0));
    if (record.outcome != "failed" && m_ride->request().reason == RideReason::ShowInRoom) {
        if (const auto game = catalog::GameId::parse(m_ride->request().game)) {
            m_pendingShow = PendingShow{*game, m_ride->request().room};
        }
    }
    m_record.rides.push_back(std::move(record));
    m_ride.reset();
    m_rideCommand.reset();
}

} // namespace rv::app
