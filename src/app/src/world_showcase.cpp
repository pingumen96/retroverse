// The hub's favourites showcase in the app (I6 step 5; ADR-0014 §3, ADR-0013 §10): the part of app::WorldSession that
// shows runtime proxies of the catalog's favourite and recent games in the hub's display case (hub_showcase.hpp: their
// content made on the pool), brings their layers in with a cover loader of their own, hovers them by the frame's ray and
// opens the directory at a proxy's card on interact. Never a plan placement: nothing of it is saved.

#include <algorithm>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/app/app_catalog.hpp>
#include <rv/app/app_logic.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/gl_device.hpp>

#include "collection_instance.hpp"
#include "cover_loader.hpp"
#include "product_ui.hpp"
#include "world_session.hpp"

namespace rv::app {

void WorldSession::startShowcase(const WorldBuild& build) {
    dropShowcase();
    m_showcaseSpec = build.showcase;
    if (!m_showcaseSpec) {
        return;
    }
    m_showcaseRecord.bound = true;
    m_showcaseWanted = true;
}

void WorldSession::requestShowcase(ProductUi& product) {
    if (!m_showcaseSpec || m_showcaseAsked || !m_showcaseWanted) {
        return;
    }
    std::shared_ptr<WorldCatalogReader> reader = product.catalog().worldReader();
    if (!reader || !m_planState) {
        return; // asked again once the catalog's world reader is open
    }
    if (!m_showcaseBox) {
        m_showcaseBox = std::make_shared<ShowcaseBox>();
        m_showcaseBox->wake = m_context.wakeCallback();
    }
    m_pool.cancel(m_showcaseGeneration);
    const std::uint64_t generation = m_showcaseGeneration.current();
    {
        const std::lock_guard lock(m_showcaseBox->mutex);
        m_showcaseBox->content.reset();
    }
    core::JobOptions options;
    options.generation = &m_showcaseGeneration;
    const Status submitted = m_pool.submit(
        [box = m_showcaseBox, cacheDir = m_options.paths.cacheDir, library = m_planState->library,
         spec = *m_showcaseSpec, reader = std::move(reader), generation](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            ShowcaseContent content = buildShowcaseContent(cacheDir, library, *reader, spec);
            std::function<void()> wake;
            {
                const std::lock_guard lock(box->mutex);
                box->content = std::move(content);
                box->generation = generation;
                wake = box->wake;
            }
            if (wake) {
                wake();
            }
        },
        options);
    if (!submitted) {
        spdlog::warn("World: the hub's showcase cannot be asked of the pool: {}", submitted.error().toString());
        return;
    }
    m_showcaseAsked = true;
    m_showcaseWanted = false;
}

void WorldSession::followShowcase(ProductUi& product) {
    if (!m_showcaseSpec) {
        return;
    }
    // A launched game's return makes the content again (a game played since is now the most recent).
    const bool launching = product.launchActive();
    if (std::exchange(m_showcaseLaunchSeen, launching) && !launching) {
        m_showcaseWanted = true;
    }
    if (m_showcaseAsked) {
        std::optional<ShowcaseContent> content;
        {
            const std::lock_guard lock(m_showcaseBox->mutex);
            if (m_showcaseBox->content && m_showcaseBox->generation == m_showcaseGeneration.current()) {
                content = std::exchange(m_showcaseBox->content, std::nullopt);
            }
        }
        if (content) {
            m_showcaseAsked = false;
            showShowcase(std::move(*content));
        }
    }
    if (!launching) {
        requestShowcase(product);
    }
}

void WorldSession::showShowcase(ShowcaseContent content) {
    // The proxies shown go (their layers released with their arrays), the new ones come with their own loader.
    m_showcaseHovered = world::kNoPick;
    m_showcasePicks.reset();
    m_showcaseLoader.reset();
    m_showcase.reset();
    m_showcaseGames = std::move(content.games);
    m_showcaseLayout = std::move(content.layout);
    ++m_showcaseRecord.builds;
    m_showcaseRecord.games.clear();
    for (const ShowcaseGame& game : m_showcaseGames) {
        m_showcaseRecord.games.push_back(game.game.toString());
    }
    spdlog::info("Frame {}: the hub's showcase holds {} favourite or recent game(s) (made in {:.1f} ms)",
                 m_context.frameIndex(), m_showcaseGames.size(), content.milliseconds);
    if (m_showcaseLayout.items.empty() || !m_showcaseSpec) {
        syncCollections();
        return;
    }
    // Keys of this build's own: the registries keep a key's first entry until the floor's scene goes (showScene()), and
    // the entries of the build before name its textures, released above.
    const std::string keyPrefix = fmt::format("{}/{}", kShowcaseKeyPrefix, m_showcaseRecord.builds);
    auto collection = CollectionInstance::create(m_showcaseLayout, m_showcaseSpec->data, m_graphics,
                                                 m_context.device().caps().uniformBufferOffsetAlignment, std::nullopt,
                                                 keyPrefix);
    if (!collection) {
        spdlog::error("World: the hub's showcase cannot be drawn: {}", collection.error().toString());
        syncCollections();
        return;
    }
    m_showcase = std::move(*collection);
    if (auto picks = world::PickIndex::build(m_showcaseLayout.pickTargets(), {})) {
        m_showcasePicks = std::make_unique<world::PickIndex>(std::move(*picks));
    } else {
        spdlog::error("World: the hub's showcase cannot be picked: {}", picks.error().toString());
    }
    syncCollections();
    auto cacheRoot = assets::textureCacheRoot(m_options.paths.cacheDir, m_planState->library.str());
    if (!cacheRoot) {
        spdlog::error("World: the texture cache: {}; the showcase's covers are not loaded", cacheRoot.error().toString());
        return;
    }
    std::vector<core::Aabb> bounds;
    for (const world::CollectionItem& item : m_showcaseLayout.items) {
        bounds.push_back(item.worldBounds());
    }
    CoverLoader::Options loader;
    loader.assetRoot = m_options.paths.assetRoot;
    loader.cacheRoot = std::move(*cacheRoot);
    loader.format = m_graphics.collectionArrayFormat();
    loader.mediaRoot = m_planState->mediaRoot;
    loader.wake = m_context.wakeCallback();
    m_showcaseLoader = std::make_unique<CoverLoader>(std::move(loader), m_pool, std::move(content.covers),
                                                     std::move(bounds));
}

void WorldSession::dropShowcase() {
    m_pool.cancel(m_showcaseGeneration);
    m_showcaseAsked = false;
    m_showcaseWanted = false;
    m_showcaseHovered = world::kNoPick;
    m_showcasePicks.reset();
    m_showcaseLoader.reset();
    m_showcase.reset();
    m_showcaseGames.clear();
    m_showcaseLayout = world::CollectionLayout{};
    m_showcaseSpec.reset();
}

void WorldSession::showcaseRound(ProductUi& product, const gl::UploadBudget& budget, bool frame) {
    if (!m_showcaseLoader || !m_showcase) {
        return;
    }
    CoverLoader::Round round;
    if (m_host.camera) {
        round.view = CoverView::of(m_host.camera());
    }
    round.paused = product.launchActive();
    round.budget = budget;
    round.frame = frame;
    round.frameIndex = m_context.frameIndex();
    CollectionInstance* collections[] = {m_showcase.get()};
    if (const Status updated = m_showcaseLoader->update(collections, round); !updated) {
        spdlog::error("Showcase's cover loader: {}", updated.error().toString());
    }
    m_showcaseRecord.frontsResident = m_showcase->arrays().residentLayers(gl::CollectionClass::Front);
}

bool WorldSession::showcaseSettled() const {
    if (!m_showcaseSpec) {
        return true;
    }
    if (m_showcaseAsked || m_showcaseWanted) {
        return false;
    }
    return !m_showcaseLoader || m_showcaseLoader->done();
}

void WorldSession::hoverShowcase(const WorldHover::Frame& frame) {
    world::PickId hovered = world::kNoPick;
    const WorldInteraction& interaction = m_interaction->interaction();
    if (m_showcasePicks && frame.ray && !m_ride && interaction.state() == WorldState::FreeRoam &&
        interaction.hovered() == world::kNoPick) {
        hovered = m_showcasePicks->pick(*frame.ray).id;
    }
    if (hovered != m_showcaseHovered) {
        m_showcaseHovered = hovered;
        if (m_showcase) {
            if (Status set = m_showcase->setHovered(hovered); !set) {
                spdlog::error("World: the showcase's hover: {}", set.error().toString());
            }
        }
    }
    const world::CollectionItem* item = m_showcaseLayout.find(m_showcaseHovered);
    m_showcaseRecord.hovered = item != nullptr ? item->game.toString() : std::string();
}

void WorldSession::openShowcaseCard(ProductUi& product) {
    const auto found = std::find_if(m_showcaseGames.begin(), m_showcaseGames.end(), [&](const ShowcaseGame& game) {
        const world::CollectionItem* item = m_showcaseLayout.find(m_showcaseHovered);
        return item != nullptr && item->game == game.game;
    });
    if (found == m_showcaseGames.end()) {
        return;
    }
    syncDirectory(product);
    ui::DirectoryGame game;
    game.gameId = found->game.toString();
    game.title = found->title;
    game.system = found->system.str();
    game.systemName = found->systemName;
    game.favorite = found->favorite;
    product.openDirectoryCard(game, "showcase");
    ++m_showcaseRecord.cardsOpened;
    m_showcaseRecord.lastCard = game.gameId;
}

} // namespace rv::app
