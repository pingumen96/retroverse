// The console rooms' last-played proxies in the app (I12 step 3, ADR-0021 §1; room_last_played.hpp): the part of
// app::WorldSession that stands each console room's last-played game in its console, as a collection of its own with a
// cover loader of its own, hides them while a game is inspected, carried or launched, and moves a room's to the game a
// launch in it played. Never a plan placement: nothing of it is saved.

#include <algorithm>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/app/app_logic.hpp>
#include <rv/app/room_last_played.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/gl_device.hpp>

#include "collection_instance.hpp"
#include "cover_loader.hpp"
#include "product_ui.hpp"
#include "world_session.hpp"

namespace rv::app {

void WorldSession::startLastPlayed(const WorldBuild& build) {
    dropLastPlayed();
    for (std::size_t index = 0; index < m_rooms.size() && index < build.rooms.size(); ++index) {
        const RoomCollection& room = build.rooms[index];
        if (!m_rooms[index].console || room.layout.items.empty()) {
            continue;
        }
        LastPlayedProxy proxy;
        proxy.room = room.room;
        proxy.game = room.lastPlayed;
        proxy.socket = *m_rooms[index].console;
        // What its collection is made with: the room's family, item prefab and theme (the floor's scene data goes).
        proxy.data.mediaFamily = room.sceneData.mediaFamily;
        proxy.data.theme = room.sceneData.theme;
        if (const world::ScenePrefab* item = room.sceneData.prefabs.find(room.sceneData.mediaFamily.item)) {
            proxy.data.prefabs.prefabs.push_back(*item);
        }
        m_lastPlayed.push_back(std::move(proxy));
    }
    for (LastPlayedProxy& proxy : m_lastPlayed) {
        showLastPlayed(proxy);
    }
    startLastPlayedLoader();
    syncCollections();
}

void WorldSession::showLastPlayed(LastPlayedProxy& proxy) {
    const ShownCollection* room = m_shown ? m_shown->find(proxy.room) : nullptr;
    std::optional<world::CollectionLayout> layout;
    if (proxy.game && room != nullptr) {
        layout = lastPlayedLayout(room->layout, *proxy.game, proxy.socket);
    }
    if (!layout) {
        // No game, or one the room no longer places: the collection that stands is kept, not drawn.
        proxy.layout = world::CollectionLayout{};
        return;
    }
    proxy.layout = std::move(*layout);
    const int alignment = m_context.device().caps().uniformBufferOffsetAlignment;
    if (proxy.collection) {
        if (auto updated = proxy.collection->applyLayout(proxy.layout, m_graphics, alignment); !updated) {
            spdlog::error("World: the last-played game of {} cannot be updated: {}", proxy.room.toString(),
                          updated.error().toString());
            proxy.layout = world::CollectionLayout{};
            return;
        }
    } else {
        auto collection = CollectionInstance::create(proxy.layout, proxy.data, m_graphics, alignment, std::nullopt,
                                                     std::string(kLastPlayedKeyPrefix) + proxy.room.toString());
        if (!collection) {
            spdlog::error("World: the last-played game of {} cannot be drawn: {}", proxy.room.toString(),
                          collection.error().toString());
            proxy.layout = world::CollectionLayout{};
            return;
        }
        proxy.collection = std::move(*collection);
    }
    ++m_lastPlayedRecord.builds;
    spdlog::info("Frame {}: the console of {} holds {}, the game played last", m_context.frameIndex(),
                 proxy.room.toString(), proxy.game->toString());
}

void WorldSession::startLastPlayedLoader() {
    m_lastPlayedLoader.reset();
    m_lastPlayedDrawn.clear();
    m_lastPlayedRecord.games.clear();
    std::vector<CoverItem> covers;
    std::vector<core::Aabb> bounds;
    std::vector<std::uint32_t> groups;
    for (std::size_t index = 0; index < m_lastPlayed.size(); ++index) {
        const LastPlayedProxy& proxy = m_lastPlayed[index];
        if (!proxy.collection || proxy.layout.items.empty()) {
            continue;
        }
        m_lastPlayedRecord.games.push_back(LastPlayedRoomRecord{proxy.room.toString(), proxy.game->toString()});
        const ShownCollection* room = m_shown ? m_shown->find(proxy.room) : nullptr;
        std::optional<CoverItem> cover = room != nullptr ? lastPlayedCover(room->covers, *proxy.game) : std::nullopt;
        const auto group = static_cast<std::uint32_t>(m_lastPlayedDrawn.size());
        m_lastPlayedDrawn.push_back(index);
        if (cover) {
            covers.push_back(std::move(*cover));
            bounds.push_back(proxy.layout.items.front().worldBounds());
            groups.push_back(group);
        }
    }
    if (covers.empty() || !m_planState) {
        return;
    }
    auto cacheRoot = assets::textureCacheRoot(m_options.paths.cacheDir, m_planState->library.str());
    if (!cacheRoot) {
        spdlog::error("World: the texture cache: {}; the last-played games' covers are not loaded",
                      cacheRoot.error().toString());
        return;
    }
    CoverLoader::Options loader;
    loader.assetRoot = m_options.paths.assetRoot;
    loader.cacheRoot = std::move(*cacheRoot);
    loader.format = m_graphics.collectionArrayFormat();
    loader.mediaRoot = m_planState->mediaRoot;
    loader.wake = m_context.wakeCallback();
    m_lastPlayedLoader = std::make_unique<CoverLoader>(std::move(loader), m_pool, std::move(covers), std::move(bounds),
                                                       std::move(groups));
}

void WorldSession::followLastPlayed(ProductUi& product) {
    if (m_lastPlayed.empty()) {
        return;
    }
    // Hidden while a game is inspected, carried, inserted or launched: the item the hand holds goes where it stands.
    const bool hidden = product.launchActive() || product.worldLaunchInFlight() ||
                        m_interaction->interaction().state() != WorldState::FreeRoam;
    if (hidden) {
        ++m_lastPlayedRecord.hiddenFrames;
    }
    bool changed = false;
    // A launch in a room that played its game: its console holds it from now on.
    if (const auto played = m_launch.takePlayed()) {
        const auto proxy =
            std::find_if(m_lastPlayed.begin(), m_lastPlayed.end(),
                         [&](const LastPlayedProxy& candidate) { return candidate.room == played->first; });
        if (proxy != m_lastPlayed.end() && proxy->game != played->second) {
            proxy->game = played->second;
            ++m_lastPlayedRecord.played;
            showLastPlayed(*proxy);
            changed = true;
        }
    }
    // A console whose socket moved (its model came, I11 step 7) takes its proxy with it.
    for (LastPlayedProxy& proxy : m_lastPlayed) {
        if (const std::optional<ConsoleSocket> socket = consoleSocket(proxy.room);
            socket && !(*socket == proxy.socket)) {
            proxy.socket = *socket;
            ++m_lastPlayedRecord.resited;
            if (proxy.game) {
                showLastPlayed(proxy);
                changed = true;
            }
        }
    }
    if (changed) {
        startLastPlayedLoader();
    }
    if (changed || hidden != m_lastPlayedHidden) {
        m_lastPlayedHidden = hidden;
        syncCollections();
    }
}

void WorldSession::lastPlayedRound(ProductUi& product, const gl::UploadBudget& budget, bool frame) {
    if (!m_lastPlayedLoader) {
        return;
    }
    std::vector<CollectionInstance*> collections;
    for (const std::size_t index : m_lastPlayedDrawn) {
        collections.push_back(m_lastPlayed[index].collection.get());
    }
    CoverLoader::Round round;
    if (m_host.camera) {
        round.view = CoverView::of(m_host.camera());
    }
    round.paused = product.launchActive();
    round.budget = budget;
    round.frame = frame;
    round.frameIndex = m_context.frameIndex();
    if (const Status updated = m_lastPlayedLoader->update(collections, round); !updated) {
        spdlog::error("Last-played games' cover loader: {}", updated.error().toString());
    }
    std::uint64_t fronts = 0;
    for (CollectionInstance* collection : collections) {
        fronts += collection->arrays().residentLayers(gl::CollectionClass::Front);
    }
    m_lastPlayedRecord.frontsResident = fronts;
}

bool WorldSession::lastPlayedSettled() const {
    return !m_lastPlayedLoader || m_lastPlayedLoader->done();
}

void WorldSession::dropLastPlayed() {
    m_lastPlayedLoader.reset();
    m_lastPlayedDrawn.clear();
    m_lastPlayed.clear();
    m_lastPlayedHidden = false;
    m_lastPlayedRecord.games.clear();
}

} // namespace rv::app
