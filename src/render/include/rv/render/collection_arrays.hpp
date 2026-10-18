#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/render/texture_array.hpp>
#include <rv/render/texture_array_layout.hpp>

namespace rv::gl {

/// Where an item's picture of one class lives: the array of its class (CollectionArrays::array) and the layer in it,
/// the values its instance record carries (scene::InstanceAttributes::frontLayer / spineLayer) once `resident`. Until
/// then the record holds kPendingLayer and the item shows the pending picture.
struct LayerSlot {
    std::uint32_t array = 0;
    std::uint32_t layer = kPendingLayer;
    /// The layer holds this item's picture (uploaded, or kept from before it was released).
    bool resident = false;

    bool operator==(const LayerSlot&) const = default;
};

/// A layer whose picture went up (CollectionArrays::uploadPending): the instance records of its item may now name it.
struct LayerUpload {
    CollectionClass cls = CollectionClass::Front;
    std::uint32_t array = 0;
    std::uint32_t layer = kPendingLayer;
    std::string key;
};

/// What one CollectionArrays::uploadPending call did.
struct LayerUploadResult {
    /// Layers uploaded, in the order they were submitted.
    std::vector<LayerUpload> completed;
    /// Queued uploads dropped because their layer was given to another key since they were submitted.
    std::size_t dropped = 0;
    /// Uploads still queued for a later frame.
    std::size_t pending = 0;
    /// Main-thread time the call took.
    std::chrono::nanoseconds elapsed{0};
};

/// The collection's texture arrays (ADR-0011 §4, §6): per class (fronts, spines) one or more gl::Texture2DArray of the
/// run's format (chooseCollectionArrayFormat: BC1 sRGB, or RGBA8 sRGB at half the size), sized to the room (its
/// placements and free slots: planArrayLayers, 2,048 layers at most per array, so that 2,049 items make two arrays of
/// each class) and grown by a GPU copy when the room outgrows them (reserve()); layer 0 of each array holds the pending
/// picture, a solid layer of the class's pending color uploaded with it and copied on the GPU into every layer the
/// array adds (Texture2DArray::fillLayers), so that no layer ever shows undefined texels. Items acquire one layer per
/// class under a key of their choosing (LayerAllocator: their own layer back after a release, else a never used one,
/// else the least recently released), submit its picture (a LayerImage of the class and format, validated), and the
/// queued pictures go up on the main thread under a per-frame budget (uploadPending(): 1 ms or 64 layers; timed as the
/// CPU section core::cpu_section::kLayerUploads in the frames that upload). The arrays' memory is recorded under
/// GpuMemoryKind::CollectionArrays. Each array keeps its address for the object's life (a growth replaces its storage
/// in place), so that a material binding it (MaterialRegistry::addLayered) samples the grown storage. Move-only; main
/// thread only, with the context current.
class CollectionArrays {
public:
    struct Desc {
        CollectionArrayFormat format = CollectionArrayFormat::Bc1Srgb;
        /// The pending picture of each class (layer 0 of its arrays): a neutral label in the family's plastic color.
        std::array<Rgb8, kCollectionClassCount> pendingColors{};
        /// Labels the arrays: "<name>.<class>[<index>]".
        std::string debugName = "CollectionArrays";
    };

    CollectionArrays() noexcept = default;

    /// The arrays of `items[class]` items per class (reserve() of each). Errors: those of Texture2DArray::create.
    [[nodiscard]] static Expected<CollectionArrays> create(const Desc& desc,
                                                           const std::array<std::size_t, kCollectionClassCount>& items);

    /// Makes the arrays of `cls` hold `items` items (planArrayLayers): an array smaller than the plan grows by a GPU
    /// copy that keeps every layer it holds, missing arrays are created with their pending layer, nothing ever shrinks.
    /// Call it at a frame boundary, before acquiring the new items' layers; an array added needs its own material.
    /// Errors: those of Texture2DArray::create and grow (the arrays keep what they had).
    [[nodiscard]] Status reserve(CollectionClass cls, std::size_t items);

    /// The layer of `key` in the arrays of `cls`: the one it holds (in use, or released and taken back with its
    /// picture), else a layer never used in the first array that has one, else the least recently released layer of
    /// the first array that has one. Errors: OutOfRange when every layer of the class is in use (reserve() first).
    [[nodiscard]] Expected<LayerSlot> acquire(CollectionClass cls, std::string_view key);
    /// Releases the layer of `key` (kept with its picture until reused); false when it holds none in use.
    bool release(CollectionClass cls, std::string_view key);
    /// The layer `key` holds in the arrays of `cls` (in use or released), if any.
    [[nodiscard]] std::optional<LayerSlot> find(CollectionClass cls, std::string_view key) const;

    /// Queues `image` for the layer `key` holds in the arrays of `cls`, for uploadPending(). Errors: NotFound when the
    /// key holds no layer in use, InvalidArgument for an image that is not a layer of the class and format
    /// (validateLayerImage); nothing is queued then.
    [[nodiscard]] Status submit(CollectionClass cls, std::string_view key, LayerImage image);

    /// Uploads queued pictures in the order they were submitted, while `budget` allows (UploadBudgetMeter: always one,
    /// then up to its layers and its time), and marks their layers resident. Uploads for a layer reassigned since are
    /// dropped. A frame without queued uploads costs nothing and records no CPU section.
    [[nodiscard]] LayerUploadResult uploadPending(const UploadBudget& budget = {});

    [[nodiscard]] CollectionArrayFormat format() const noexcept { return m_format; }
    /// Arrays of `cls`.
    [[nodiscard]] std::size_t arrayCount(CollectionClass cls) const noexcept;
    /// Array `index` of `cls` (index < arrayCount(cls)); its address never changes.
    [[nodiscard]] const Texture2DArray& array(CollectionClass cls, std::size_t index) const;
    /// Items the arrays of `cls` can hold now.
    [[nodiscard]] std::size_t itemCapacity(CollectionClass cls) const noexcept;
    /// Layers of `cls` held by a key in use, and how many of them hold their picture.
    [[nodiscard]] std::size_t usedLayers(CollectionClass cls) const noexcept;
    [[nodiscard]] std::size_t residentLayers(CollectionClass cls) const noexcept;
    /// Uploads queued (stale ones included until uploadPending drops them).
    [[nodiscard]] std::size_t pendingUploads() const noexcept { return m_queue.size(); }
    /// Bytes of every array (what the ledger records under CollectionArrays for them).
    [[nodiscard]] std::size_t estimatedBytes() const noexcept;

private:
    struct Array {
        Texture2DArray texture;
        LayerAllocator layers;
    };
    struct Upload {
        CollectionClass cls = CollectionClass::Front;
        std::uint32_t array = 0;
        std::uint32_t layer = 0;
        std::uint32_t generation = 0;
        std::string key;
        LayerImage image;
    };

    [[nodiscard]] Status addArray(CollectionClass cls, int layers);
    [[nodiscard]] std::vector<std::unique_ptr<Array>>& arrays(CollectionClass cls) noexcept {
        return m_arrays[toIndex(cls)];
    }
    [[nodiscard]] const std::vector<std::unique_ptr<Array>>& arrays(CollectionClass cls) const noexcept {
        return m_arrays[toIndex(cls)];
    }

    CollectionArrayFormat m_format = CollectionArrayFormat::Bc1Srgb;
    std::array<Rgb8, kCollectionClassCount> m_pendingColors{};
    std::string m_debugName;
    /// Per class; each array behind a pointer so that its address survives the vector growing.
    std::array<std::vector<std::unique_ptr<Array>>, kCollectionClassCount> m_arrays;
    std::deque<Upload> m_queue;
    UploadBudgetMeter m_meter;
};

} // namespace rv::gl
