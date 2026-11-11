#include "world_shelf_labels.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/assets/bc1.hpp>
#include <rv/assets/console_content.hpp>
#include <rv/assets/label_atlas.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/assets/text_raster.hpp>

#include "world_signs.hpp"

namespace rv::app {

namespace {

/// Behind the covers, whose priority falls with their rank from the eye (CoverLoader: 2 - rank, a few thousand down on
/// a floor of the real library): a ride's view waits for the covers in view, never for a label. Above the gallery's.
constexpr std::int64_t kJobPriority = -500'000;

} // namespace

struct WorldShelfLabels::Shared {
    struct Finished {
        std::string objectId;
        std::optional<assets::ImageData> picture;
        std::optional<assets::CompressedTexture> blocks;
        std::string error;
    };
    std::mutex mutex;
    std::vector<Finished> finished;
    std::once_flag fontsLoaded;
    std::optional<assets::FontFace> bold;
    std::optional<assets::FontFace> regular;
    std::optional<Error> fontError;
};

WorldShelfLabels::WorldShelfLabels(core::WorkerPool& pool, std::filesystem::path assetRoot)
    : m_pool(pool), m_assetRoot(std::move(assetRoot)), m_shared(std::make_shared<Shared>()) {}

WorldShelfLabels::~WorldShelfLabels() {
    m_pool.cancel(m_generation);
}

std::size_t WorldShelfLabels::atlasesShown() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(m_atlases.begin(), m_atlases.end(), [](const auto& entry) { return entry.second.applied; }));
}

std::uint64_t WorldShelfLabels::bytes() const noexcept {
    std::uint64_t total = 0;
    for (const auto& [id, atlas] : m_atlases) {
        total += atlas.texture ? atlas.bytes : 0;
    }
    return total;
}

void WorldShelfLabels::takeOff(gl::MaterialRegistry& materials) {
    for (auto& [id, atlas] : m_atlases) {
        if (atlas.applied && atlas.handle && atlas.handle->value < materials.size() &&
            materials.material(*atlas.handle).key == atlas.materialKey) {
            static_cast<void>(materials.setTextureOverride(*atlas.handle, gl::MaterialTextureSlot::BaseColor, 0));
        }
        atlas.applied = false;
    }
}

void WorldShelfLabels::ask(Atlas& atlas) {
    atlas.pending = true;
    core::JobOptions options;
    options.priority = kJobPriority;
    options.generation = &m_generation;
    const Status submitted = m_pool.submit(
        [shared = m_shared, assetRoot = m_assetRoot, objectId = atlas.objectId, tabs = atlas.tabs,
         tabTexts = atlas.tabTexts, captionTexts = atlas.captionTexts,
         compressed = atlas.compressed](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            Shared::Finished finished{objectId, std::nullopt, std::nullopt, {}};
            std::call_once(shared->fontsLoaded, [&] {
                auto bold = assets::FontFace::loadBundled(assetRoot, assets::kLabelTitleFontPath);
                auto regular = assets::FontFace::loadBundled(assetRoot, assets::kLabelDetailFontPath);
                if (bold && regular) {
                    shared->bold = std::move(*bold);
                    shared->regular = std::move(*regular);
                } else {
                    shared->fontError = bold ? regular.error() : bold.error();
                }
            });
            if (shared->fontError) {
                finished.error = shared->fontError->toString();
            } else {
                assets::ImageData picture =
                    tabs ? assets::makeTabAtlas(tabTexts, *shared->bold)
                         : assets::makeCaptionAtlas(captionTexts, *shared->bold, *shared->regular);
                if (!picture.isValid()) {
                    finished.error = "no atlas for its texts";
                } else if (compressed) {
                    assets::CompressedTexture blocks;
                    blocks.srgb = true;
                    blocks.format = assets::TextureBlocks::Bc1Srgb;
                    for (const assets::ImageData& level : assets::buildMipChain(picture)) {
                        if (job.cancelled()) {
                            return;
                        }
                        blocks.levels.push_back(
                            assets::CompressedLevel{level.width, level.height, assets::encodeBc1(level)});
                    }
                    finished.blocks = std::move(blocks);
                } else {
                    finished.picture = std::move(picture);
                }
            }
            const std::lock_guard lock(shared->mutex);
            shared->finished.push_back(std::move(finished));
        },
        options);
    if (!submitted) {
        atlas.pending = false;
        ++m_failed;
        spdlog::warn("World: the atlas of {} cannot be asked for: {}", atlas.objectId, submitted.error().toString());
    }
}

bool WorldShelfLabels::sameObjects(const scene::SceneDescription& description, std::string_view prefix,
                                   const std::vector<scene::SceneObject>& wanted) {
    std::size_t matched = 0;
    for (const scene::SceneObject& object : description.objects) {
        if (!object.id.starts_with(prefix)) {
            continue;
        }
        if (matched >= wanted.size() || !(object == wanted[matched])) {
            return false;
        }
        ++matched;
    }
    return matched == wanted.size();
}

bool WorldShelfLabels::place(const Frame& frame, std::string_view prefix, std::vector<scene::SceneObject> wanted,
                             bool& replaced) {
    if (sameObjects(*frame.description, prefix, wanted)) {
        return true;
    }
    SceneObjectsChange change;
    change.prefix = std::string(prefix);
    change.objects = std::move(wanted);
    if (!frame.replaceObjects(change)) {
        spdlog::warn("World: the shelves' labels under {} cannot be put in the scene", prefix);
        return false;
    }
    replaced = true;
    return true;
}

void WorldShelfLabels::rescan(const Frame& frame) {
    // What the scene should hold, room by room.
    std::vector<RoomLabels> rooms;
    std::set<std::string, std::less<>> wantedRooms;
    m_tabs = 0;
    m_captions = 0;
    m_sections = 0;
    for (const ShelfLabels* labels : frame.labels) {
        if (labels == nullptr) {
            continue;
        }
        RoomLabels room;
        room.room = labels->room.toString();
        if (std::optional<scene::SceneObject> tabs = shelfTabsObject(*labels)) {
            Atlas atlas;
            atlas.objectId = tabs->id;
            atlas.tabs = true;
            atlas.tabTexts = tabTexts(*labels);
            atlas.compressed = frame.compressed;
            room.atlases.push_back(std::move(atlas));
            room.tabs.push_back(std::move(*tabs));
            m_tabs += labels->tabs.size();
        }
        if (std::optional<scene::SceneObject> captions = shelfCaptionsObject(*labels)) {
            Atlas atlas;
            atlas.objectId = captions->id;
            atlas.tabs = false;
            atlas.captionTexts = captionTexts(*labels);
            atlas.compressed = frame.compressed;
            room.atlases.push_back(std::move(atlas));
            room.captions.push_back(std::move(*captions));
            m_captions += labels->captions.size();
        }
        for (const ShelfSectionSign& sign : labels->sections) {
            room.signs.push_back(sectionSignObject(*labels, sign));
        }
        m_sections += labels->sections.size();
        wantedRooms.insert(room.room);
        rooms.push_back(std::move(room));
    }
    // The rooms whose labels the scene holds and no longer wants (a room the floor shown lost): taken away.
    for (const scene::SceneObject& object : frame.description->objects) {
        for (const std::string_view prefix : {kTabsPrefix, kCaptionsPrefix, kSectionSignPrefix}) {
            if (!object.id.starts_with(prefix)) {
                continue;
            }
            const std::string_view rest = std::string_view(object.id).substr(prefix.size());
            std::string room(rest.substr(0, rest.rfind('/')));
            if (!wantedRooms.contains(room)) {
                wantedRooms.insert(room);
                RoomLabels gone;
                gone.room = std::move(room);
                rooms.push_back(std::move(gone));
            }
        }
    }
    // The atlases: a room's whose objects stand as wanted at once, the others' as their room is placed, one room a
    // frame (building a floor's plates at once held the frame a ride arrives in).
    takeOff(*frame.materials);
    m_previous = std::move(m_atlases);
    m_atlases.clear();
    m_queue.clear();
    for (RoomLabels& room : rooms) {
        const bool standing =
            sameObjects(*frame.description, labelsPrefix(kTabsPrefix, room.room), room.tabs) &&
            sameObjects(*frame.description, labelsPrefix(kCaptionsPrefix, room.room), room.captions) &&
            sameObjects(*frame.description, labelsPrefix(kSectionSignPrefix, room.room), room.signs);
        if (standing) {
            adopt(frame, room);
        } else {
            m_queue.push_back(std::move(room));
        }
    }
    if (m_queue.empty()) {
        m_previous.clear();
    }
}

void WorldShelfLabels::adopt(const Frame& frame, RoomLabels& room) {
    // Kept when their texts are the same, else drawn again.
    for (Atlas& atlas : room.atlases) {
        atlas.handle = objectPictureMaterial(atlas.objectId, *frame.instance, *frame.materials);
        if (atlas.handle) {
            atlas.materialKey = frame.materials->material(*atlas.handle).key;
        }
        const auto old = m_previous.find(atlas.objectId);
        if (old != m_previous.end() && old->second.sameContent(atlas) && (old->second.texture || old->second.pending)) {
            atlas.texture = std::move(old->second.texture);
            atlas.bytes = old->second.bytes;
            atlas.pending = old->second.pending;
        }
        const std::string id = atlas.objectId;
        Atlas& kept = m_atlases.insert_or_assign(id, std::move(atlas)).first->second;
        if (!kept.texture && !kept.pending && kept.handle) {
            ask(kept);
        }
    }
}

void WorldShelfLabels::placeRooms(const Frame& frame) {
    if (frame.hold && !frame.settle) {
        return;
    }
    bool placed = false;
    bool signsReplaced = false;
    while (!m_queue.empty() && (frame.settle || !placed)) {
        if (frame.frozen || !frame.replaceObjects) {
            return; // again once the editor lets go
        }
        RoomLabels room = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
        bool replaced = false;
        bool signs = false;
        if (!place(frame, labelsPrefix(kTabsPrefix, room.room), std::move(room.tabs), replaced) ||
            !place(frame, labelsPrefix(kCaptionsPrefix, room.room), std::move(room.captions), replaced) ||
            !place(frame, labelsPrefix(kSectionSignPrefix, room.room), std::move(room.signs), signs)) {
            continue; // logged: its labels stay out
        }
        placed = placed || replaced || signs;
        signsReplaced = signsReplaced || signs;
        adopt(frame, room);
    }
    if (signsReplaced && frame.signsChanged) {
        frame.signsChanged();
    }
    if (m_queue.empty() && placed) {
        m_previous.clear();
        spdlog::info("World: {} letter tab(s), {} caption card(s), {} section sign(s) on the floor's shelves", m_tabs,
                     m_captions, m_sections);
    }
}

void WorldShelfLabels::update(const Frame& frame) {
    if (frame.description == nullptr || frame.instance == nullptr || frame.materials == nullptr) {
        return;
    }
    if (m_dirty || m_revision != frame.revision) {
        m_dirty = false;
        m_revision = frame.revision;
        rescan(frame);
    }
    placeRooms(frame);
    if (frame.settle) {
        const auto waiting = [&] {
            std::size_t pending = 0;
            for (const auto& [id, atlas] : m_atlases) {
                pending += atlas.pending ? 1 : 0;
            }
            std::size_t arrived = 0;
            const std::lock_guard lock(m_shared->mutex);
            for (const Shared::Finished& done : m_shared->finished) {
                const auto atlas = m_atlases.find(done.objectId);
                arrived += atlas != m_atlases.end() && atlas->second.pending ? 1 : 0;
            }
            return arrived < pending;
        };
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(kSettleSeconds);
        while (waiting() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::vector<Shared::Finished> finished;
    {
        const std::lock_guard lock(m_shared->mutex);
        const std::size_t take =
            frame.settle ? m_shared->finished.size() : std::min(kUploadsPerFrame, m_shared->finished.size());
        finished.assign(std::make_move_iterator(m_shared->finished.begin()),
                        std::make_move_iterator(m_shared->finished.begin() + static_cast<std::ptrdiff_t>(take)));
        m_shared->finished.erase(m_shared->finished.begin(),
                                 m_shared->finished.begin() + static_cast<std::ptrdiff_t>(take));
    }
    for (Shared::Finished& done : finished) {
        const auto found = m_atlases.find(done.objectId);
        if (found == m_atlases.end() || !found->second.pending) {
            continue;
        }
        Atlas& atlas = found->second;
        atlas.pending = false;
        if (!done.picture && !done.blocks) {
            ++m_failed;
            spdlog::warn("World: the atlas of {} is not drawn: {}", done.objectId, done.error);
            continue;
        }
        gl::TextureDesc desc;
        desc.colorSpace = gl::ColorSpace::Srgb;
        desc.mipmaps = true;
        desc.sampling.wrap = gl::TextureWrap::ClampToEdge;
        desc.sampling.filter = gl::TextureFilter::Trilinear;
        desc.memoryKind = gl::GpuMemoryKind::LabelTextures;
        auto texture = done.blocks ? gl::Texture2D::createCompressed(*done.blocks, desc, "Shelf labels")
                                   : gl::Texture2D::create(*done.picture, desc, "Shelf labels");
        if (!texture) {
            ++m_failed;
            spdlog::warn("World: the atlas of {} cannot be uploaded: {}", done.objectId, texture.error().toString());
            continue;
        }
        atlas.bytes = texture->estimatedBytes();
        atlas.texture = std::move(*texture);
    }
    for (auto& [id, atlas] : m_atlases) {
        if (atlas.applied || !atlas.texture || !atlas.handle) {
            continue;
        }
        if (Status on = frame.materials->setTextureOverride(*atlas.handle, gl::MaterialTextureSlot::BaseColor,
                                                            atlas.texture->id());
            !on) {
            spdlog::warn("World: the labels of {} keep a blank face: {}", id, on.error().toString());
            atlas.handle.reset();
            continue;
        }
        atlas.applied = true;
    }
}

void WorldShelfLabels::release(gl::MaterialRegistry* materials) {
    m_pool.cancel(m_generation);
    if (materials != nullptr) {
        takeOff(*materials);
    }
    m_atlases.clear();
    m_previous.clear();
    m_queue.clear();
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->finished.clear();
    }
    m_dirty = true;
    m_revision.reset();
}

} // namespace rv::app
