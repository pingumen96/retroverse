#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or RmlUi header is included.
//
// The cover loader's order (I3 step 12; ADR-0011 §6, §8): which of the room's items the loader makes next on the worker
// pool and uploads next on the main thread, and when what the frame shows is in. Three ranks, best first: the focused
// (or pulled-out, or inspected) item; the items in view, by their projected size (the nearest and largest first); the
// rest of the room by their distance from the walker. CoverSchedule keeps every item's stage (waiting, being made,
// made, uploading, done), ranks again when the view changes, hands out the next items to make and to upload (none while
// a launched game runs: the pause of ADR-0012 §6), and records the order in which the items arrived (the run report's
// check that covers arrive nearest first). HeroLru is the hero textures' least-recently-used set (ADR-0011 §8: 8 of
// them; the one shown is never evicted). Main thread only, like the loader that owns them.
//
// Since I4 step 5a one loader serves a floor's rooms (ADR-0013 §9): each item belongs to a group (its room), and "the
// view" is the items of the groups the view names (the room that holds the eye, and those whose doorway the frustum
// meets) that meet a frustum; the other items come by their distance.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/assets/cover_fit.hpp>
#include <rv/assets/image_data.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/scene/camera.hpp>

namespace rv::app {

/// Where the frame looks from: the walker's eye and the frusta of the frame's views (the window's, and the screenshot's
/// when its aspect differs). An item is in view when its box meets one of them (Frustum::intersects, conservative) and
/// its group is one the view sees (`groups`: a flag per group, 0 for one out of sight; empty: every group).
struct CoverView {
    glm::vec3 eye{0.0f};
    std::vector<scene::Frustum> frusta;
    std::vector<std::uint8_t> groups;

    /// The view sees the items of `group`.
    [[nodiscard]] bool seesGroup(std::uint32_t group) const noexcept {
        return groups.empty() || (group < groups.size() && groups[group] != 0);
    }

    /// The view of `camera` (its aspect), and also of `camera` at `otherAspect` when that differs (a screenshot's).
    [[nodiscard]] static CoverView of(const scene::Camera& camera, std::optional<float> otherAspect = std::nullopt);

    bool operator==(const CoverView& other) const noexcept;
};

/// The rank of an item in the loader's order (lower first).
enum class CoverRank : std::uint8_t {
    Focused = 0, ///< the focused, pulled-out or inspected item (its layers and its hero texture first)
    InView = 1,  ///< in a frustum of the view: the largest projected size first
    Rest = 2,    ///< out of view: the nearest first
};

/// "focused", "in-view", "rest".
[[nodiscard]] std::string_view toString(CoverRank rank) noexcept;

/// An item's place in the order: its rank, then `order` (lower first: minus the projected size for InView, the
/// distance for the others), then its index (deterministic among equals).
struct CoverPriority {
    CoverRank rank = CoverRank::Rest;
    float order = 0.0f;
    std::size_t item = 0;

    [[nodiscard]] bool operator<(const CoverPriority& other) const noexcept;
};

/// True when `bounds` meets a frustum of `view` (an empty box never does) and `group` is one the view sees.
[[nodiscard]] bool coverInView(const core::Aabb& bounds, const CoverView& view, std::uint32_t group = 0) noexcept;
/// The distance from the eye to the box (0 inside it).
[[nodiscard]] float coverDistance(const core::Aabb& bounds, const glm::vec3& eye) noexcept;
/// The priority of item `item` with `bounds` under `view`; `focused` puts it first whatever the view. The projected
/// size of an item in view is its box's half diagonal over its distance from the eye (at least 1 cm).
[[nodiscard]] CoverPriority coverPriority(std::size_t item, const core::Aabb& bounds, const CoverView& view,
                                          bool focused, std::uint32_t group = 0) noexcept;

/// One item that arrived (all of its layers resident or given up), in arrival order.
struct CoverArrival {
    std::size_t item = 0;
    /// Its rank and distance from the eye when it arrived.
    CoverRank rank = CoverRank::Rest;
    float distance = 0.0f;
};

/// The stages of every item of the room on their way to the arrays (see the file comment). Items are the indices of the
/// room's collection (CollectionInstance's item order).
class CoverSchedule {
public:
    enum class Stage : std::uint8_t {
        Waiting,   ///< to be made (queued)
        Making,    ///< its job runs on the pool
        Made,      ///< its layers are made and wait for their upload
        Uploading, ///< its layers are submitted to the arrays
        Done,      ///< every layer resident (or given up)
    };

    /// `bounds`: each item's box in the room (world::CollectionItem::worldBounds); `groups`: each item's group (its
    /// room on the floor; none: every item in group 0). Every item starts Waiting.
    explicit CoverSchedule(std::vector<core::Aabb> bounds, std::vector<std::uint32_t> groups = {});

    [[nodiscard]] std::size_t size() const noexcept { return m_bounds.size(); }
    [[nodiscard]] Stage stage(std::size_t item) const { return m_stage.at(item); }
    /// The item's rank under the current view and focus.
    [[nodiscard]] CoverRank rank(std::size_t item) const { return m_priority.at(item).rank; }

    /// The frame's view: the items are ranked again when it differs from the last one.
    void setView(const CoverView& view);
    /// The focused item (none: none), ranked first.
    void setFocused(std::optional<std::size_t> item);
    [[nodiscard]] std::optional<std::size_t> focused() const noexcept { return m_focused; }
    /// While a launched game runs (ADR-0012 §6) nothing is handed out to make or to upload; jobs already running
    /// finish and their results wait.
    void setPaused(bool paused) noexcept { m_paused = paused; }
    [[nodiscard]] bool paused() const noexcept { return m_paused; }

    /// Up to `count` waiting items, best first, now Making (none while paused).
    [[nodiscard]] std::vector<std::size_t> takeToMake(std::size_t count);
    /// Item `item`'s job came back with `layers` layers to upload (0: nothing to upload, it is Done at once). A job
    /// whose item is not Making (requeued meanwhile, say) is ignored: false.
    bool made(std::size_t item, unsigned layers);
    /// Made items, best first, while their layers stay within `maxLayers` (always one when any is made and
    /// `maxLayers` > 0), now Uploading (none while paused).
    [[nodiscard]] std::vector<std::size_t> takeToUpload(std::size_t maxLayers);
    /// One layer of an Uploading item is resident, or will not be (dropped, refused): at its last it is Done.
    void layerDone(std::size_t item);
    /// A Making item goes back to waiting (its job could not be submitted).
    void giveBack(std::size_t item);
    /// Queues a Done item again (a retry after the catalog's refresh, ADR-0011 §6); false for one on its way already.
    bool requeue(std::size_t item);

    /// Every item is Done.
    [[nodiscard]] bool done() const noexcept { return m_doneCount == m_bounds.size(); }
    [[nodiscard]] std::size_t doneCount() const noexcept { return m_doneCount; }
    /// Every item in the current view (and the focused one) is Done.
    [[nodiscard]] bool viewDone() const noexcept;
    /// Items in the current view, and those of them not Done.
    [[nodiscard]] std::size_t inViewCount() const noexcept { return m_inViewCount; }
    [[nodiscard]] std::size_t inViewPending() const noexcept;
    [[nodiscard]] bool inView(std::size_t item) const { return m_inView.at(item) != 0; }
    /// Items Made and waiting for their upload, and the layers they hold.
    [[nodiscard]] std::size_t madeCount() const noexcept { return m_made.size(); }
    [[nodiscard]] std::size_t madeLayers() const noexcept { return m_madeLayers; }
    /// Items whose job runs.
    [[nodiscard]] std::size_t makingCount() const noexcept { return m_makingCount; }

    /// The first arrival of every item that arrived, in order (a requeued item is not listed again).
    [[nodiscard]] std::span<const CoverArrival> arrivals() const noexcept { return m_arrivals; }

private:
    void rank();
    [[nodiscard]] CoverPriority priorityOf(std::size_t item) const;
    void finish(std::size_t item);

    std::vector<core::Aabb> m_bounds;
    std::vector<std::uint32_t> m_groups;
    std::vector<Stage> m_stage;
    std::vector<unsigned> m_layersLeft;
    std::vector<std::uint8_t> m_inView;
    std::vector<std::uint8_t> m_arrived;
    std::vector<CoverPriority> m_priority;
    /// Waiting and Made items, each sorted best last (taken from the back) while its flag says so.
    std::vector<std::size_t> m_waiting;
    std::vector<std::size_t> m_made;
    bool m_waitingSorted = false;
    bool m_madeSorted = false;
    std::size_t m_madeLayers = 0;
    std::size_t m_makingCount = 0;
    std::size_t m_doneCount = 0;
    std::size_t m_inViewCount = 0;
    std::optional<CoverView> m_view;
    std::optional<std::size_t> m_focused;
    bool m_paused = false;
    std::vector<CoverArrival> m_arrivals;
};

/// The longest side of a hero texture (ADR-0011 §8).
inline constexpr int kHeroMaxSide = 1024;

/// The size of the hero picture of a cover of `sourceWidth` x `sourceHeight`: the front label's aspect (256 x 188, as
/// the label quad's texture coordinates span it; the label quad's own shape for a family whose covers are fitted to it,
/// `label`, ADR-0013 §4), as wide as the cover's pixels need, the cover fitted into it without stretching
/// (assets::fitCover's rules), within ADR-0011 §8's bounds whatever the shape: the long side at most `maxSide`, and
/// at most the pixels of the layer's shape at `maxSide` wide (1,024 x 752 = 770,048: the set of 8 within 32 MiB);
/// at least 16 x 12. A 680 x 497 cover gives 680 x 499, a 476 x 864 portrait one 1024 x 752 (its height kept up to
/// the cap), a 2,050 x 1,440 scan 1024 x 752; at the jewel case's square label a large scan gives 877 x 877, at the
/// keep case's 123 x 178 one 707 x 1023.
[[nodiscard]] glm::ivec2 heroPictureSize(int sourceWidth, int sourceHeight, int maxSide = kHeroMaxSide,
                                         const assets::LabelAspect& label = {}) noexcept;

/// The hero picture of a cover file's bytes (on a worker): decoded (PNG or JPEG), fitted to heroPictureSize() (of the
/// label's shape: the quad then shows it as it is), RGBA8 sRGB, top row first. Errors: those of assets::decodeImage and
/// assets::fitCover, with `context` (it names the user's file: log it at debug level only).
[[nodiscard]] Expected<assets::ImageData> makeHeroPicture(std::span<const std::byte> bytes, std::string_view context,
                                                          int maxSide = kHeroMaxSide,
                                                          const assets::LabelAspect& label = {});

/// The hero textures' set (ADR-0011 §8): at most `capacity` keys (GameIds), least recently used first out. Main thread
/// only.
class HeroLru {
public:
    static constexpr std::size_t kDefaultCapacity = 8;

    explicit HeroLru(std::size_t capacity = kDefaultCapacity);

    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] std::size_t size() const noexcept { return m_keys.size(); }
    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    /// The keys, least recently used first.
    [[nodiscard]] std::span<const std::string> keys() const noexcept { return m_keys; }

    /// `key` becomes the most recently used; false when it is not held.
    bool touch(std::string_view key);
    /// Holds `key` as the most recently used (touched when held already). When the set is full, the least recently
    /// used key that is not `pinned` makes room and is returned (the caller frees its texture); with every key pinned
    /// nothing is evicted and `key` is not held: returns `key` itself.
    [[nodiscard]] std::optional<std::string> insert(std::string key, std::span<const std::string> pinned = {});
    /// Forgets `key`; false when it is not held.
    bool erase(std::string_view key);

private:
    std::size_t m_capacity;
    std::vector<std::string> m_keys;
};

} // namespace rv::app
