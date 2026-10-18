#include <rv/render/collection_arrays.hpp>

#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/cpu_timers.hpp>

namespace rv::gl {

Expected<CollectionArrays> CollectionArrays::create(const Desc& desc,
                                                    const std::array<std::size_t, kCollectionClassCount>& items) {
    CollectionArrays result;
    result.m_format = desc.format;
    result.m_pendingColors = desc.pendingColors;
    result.m_debugName = desc.debugName;
    for (const CollectionClass cls : kCollectionClasses) {
        if (auto reserved = result.reserve(cls, items[toIndex(cls)]); !reserved) {
            return propagate(reserved.error());
        }
    }
    spdlog::info("Collection arrays '{}' ({}): fronts for {} item(s) in {} array(s), spines for {} in {}, {:.1f} MiB",
                 result.m_debugName, toString(result.m_format), result.itemCapacity(CollectionClass::Front),
                 result.arrayCount(CollectionClass::Front), result.itemCapacity(CollectionClass::Spine),
                 result.arrayCount(CollectionClass::Spine),
                 static_cast<double>(result.estimatedBytes()) / (1024.0 * 1024.0));
    return result;
}

Status CollectionArrays::addArray(CollectionClass cls, int layers) {
    std::vector<std::unique_ptr<Array>>& list = arrays(cls);
    const std::string name = fmt::format("{}.{}[{}]", m_debugName, toString(cls), list.size());
    auto texture =
        Texture2DArray::create(m_format, layerSize(cls, m_format), layerLevelCount(cls, m_format), layers, name);
    if (!texture) {
        return propagate(texture.error());
    }
    const LayerImage pending = makeSolidLayer(cls, m_format, m_pendingColors[toIndex(cls)]);
    if (auto uploaded = texture->uploadLayer(static_cast<int>(kPendingLayer), pending.levels); !uploaded) {
        return uploaded;
    }
    // Every other layer starts as the pending picture too: a layer read before its picture arrives never shows
    // undefined texels, and the driver commits the storage now rather than during the per-frame uploads.
    if (auto filled = texture->fillLayers(static_cast<int>(kPendingLayer), 1, layers - 1); !filled) {
        return filled;
    }
    auto entry = std::make_unique<Array>();
    entry->texture = std::move(*texture);
    entry->layers = LayerAllocator(layers);
    list.push_back(std::move(entry));
    return ok();
}

Status CollectionArrays::reserve(CollectionClass cls, std::size_t items) {
    const std::vector<int> plan = planArrayLayers(items);
    std::vector<std::unique_ptr<Array>>& list = arrays(cls);
    for (std::size_t index = 0; index < plan.size(); ++index) {
        if (index >= list.size()) {
            if (auto added = addArray(cls, plan[index]); !added) {
                return added;
            }
            continue;
        }
        Array& entry = *list[index];
        const int before = entry.texture.layerCount();
        if (plan[index] <= before) {
            continue;
        }
        if (auto grown = entry.texture.grow(plan[index]); !grown) {
            return grown;
        }
        entry.layers.grow(plan[index]);
        // The new layers show the pending picture until theirs arrive, like those of a new array.
        if (auto filled = entry.texture.fillLayers(static_cast<int>(kPendingLayer), before, plan[index] - before);
            !filled) {
            return filled;
        }
        spdlog::info("Collection array '{}' grown from {} to {} layers by a GPU copy", entry.texture.debugName(),
                     before, plan[index]);
    }
    return ok();
}

Expected<LayerSlot> CollectionArrays::acquire(CollectionClass cls, std::string_view key) {
    std::vector<std::unique_ptr<Array>>& list = arrays(cls);
    const auto take = [&](std::size_t index) -> std::optional<LayerSlot> {
        const std::optional<LayerAllocator::Acquired> acquired = list[index]->layers.acquire(key);
        if (!acquired) {
            return std::nullopt;
        }
        return LayerSlot{static_cast<std::uint32_t>(index), acquired->layer, acquired->resident};
    };
    // A key that holds a layer keeps it (with its picture, when it was only released).
    for (std::size_t index = 0; index < list.size(); ++index) {
        if (list[index]->layers.layerOf(key)) {
            if (const std::optional<LayerSlot> slot = take(index)) {
                return *slot;
            }
        }
    }
    // Then a layer never used, before reusing one whose picture an item that left the room may still want.
    for (std::size_t index = 0; index < list.size(); ++index) {
        if (list[index]->layers.neverUsedCount() > 0) {
            if (const std::optional<LayerSlot> slot = take(index)) {
                return *slot;
            }
        }
    }
    for (std::size_t index = 0; index < list.size(); ++index) {
        if (list[index]->layers.releasedCount() > 0) {
            if (const std::optional<LayerSlot> slot = take(index)) {
                return *slot;
            }
        }
    }
    return makeError(
        ErrorCode::OutOfRange,
        fmt::format("every {} layer is in use ({} item(s)): reserve more", toString(cls), itemCapacity(cls)),
        std::string(key));
}

bool CollectionArrays::release(CollectionClass cls, std::string_view key) {
    for (const std::unique_ptr<Array>& entry : arrays(cls)) {
        if (entry->layers.release(key)) {
            return true;
        }
    }
    return false;
}

std::optional<LayerSlot> CollectionArrays::find(CollectionClass cls, std::string_view key) const {
    const std::vector<std::unique_ptr<Array>>& list = arrays(cls);
    for (std::size_t index = 0; index < list.size(); ++index) {
        if (const std::optional<std::uint32_t> layer = list[index]->layers.layerOf(key)) {
            return LayerSlot{static_cast<std::uint32_t>(index), *layer, list[index]->layers.resident(*layer)};
        }
    }
    return std::nullopt;
}

Status CollectionArrays::submit(CollectionClass cls, std::string_view key, LayerImage image) {
    if (auto valid = validateLayerImage(image, cls, m_format); !valid) {
        Error error = valid.error();
        error.context = fmt::format("{} ({})", error.context, key);
        return propagate(std::move(error));
    }
    const std::vector<std::unique_ptr<Array>>& list = arrays(cls);
    for (std::size_t index = 0; index < list.size(); ++index) {
        const LayerAllocator& layers = list[index]->layers;
        const std::optional<std::uint32_t> layer = layers.layerOf(key);
        if (!layer) {
            continue;
        }
        if (!layers.inUse(*layer)) {
            break;
        }
        m_queue.push_back(Upload{cls, static_cast<std::uint32_t>(index), *layer, layers.generation(*layer),
                                 std::string(key), std::move(image)});
        return ok();
    }
    return makeError(ErrorCode::NotFound, fmt::format("the key holds no {} layer in use", toString(cls)),
                     std::string(key));
}

LayerUploadResult CollectionArrays::uploadPending(const UploadBudget& budget) {
    LayerUploadResult result;
    if (m_queue.empty()) {
        return result;
    }
    const core::CpuSectionScope timing(core::cpu_section::kLayerUploads);
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    m_meter.beginFrame(budget);
    while (!m_queue.empty() && m_meter.mayUploadNext(Clock::now() - start)) {
        Upload upload = std::move(m_queue.front());
        m_queue.pop_front();
        Array& entry = *arrays(upload.cls)[upload.array];
        if (entry.layers.generation(upload.layer) != upload.generation) {
            ++result.dropped;
            continue;
        }
        const Clock::time_point layerStart = Clock::now();
        if (auto uploaded = entry.texture.uploadLayer(static_cast<int>(upload.layer), upload.image.levels); !uploaded) {
            // Validated when submitted: only a broken invariant gets here. The item keeps the pending picture.
            spdlog::error("Collection layer upload failed: {}", uploaded.error().toString());
            continue;
        }
        m_meter.recordLayer(Clock::now() - layerStart);
        entry.layers.markResident(upload.layer, upload.generation);
        result.completed.push_back(LayerUpload{upload.cls, upload.array, upload.layer, std::move(upload.key)});
    }
    result.pending = m_queue.size();
    result.elapsed = Clock::now() - start;
    return result;
}

std::size_t CollectionArrays::arrayCount(CollectionClass cls) const noexcept {
    return arrays(cls).size();
}

const Texture2DArray& CollectionArrays::array(CollectionClass cls, std::size_t index) const {
    return arrays(cls).at(index)->texture;
}

std::size_t CollectionArrays::itemCapacity(CollectionClass cls) const noexcept {
    std::size_t items = 0;
    for (const std::unique_ptr<Array>& entry : arrays(cls)) {
        items += static_cast<std::size_t>(entry->layers.capacity() - 1);
    }
    return items;
}

std::size_t CollectionArrays::usedLayers(CollectionClass cls) const noexcept {
    std::size_t used = 0;
    for (const std::unique_ptr<Array>& entry : arrays(cls)) {
        used += entry->layers.usedCount();
    }
    return used;
}

std::size_t CollectionArrays::residentLayers(CollectionClass cls) const noexcept {
    std::size_t resident = 0;
    for (const std::unique_ptr<Array>& entry : arrays(cls)) {
        resident += entry->layers.residentUsedCount();
    }
    return resident;
}

std::size_t CollectionArrays::estimatedBytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& list : m_arrays) {
        for (const std::unique_ptr<Array>& entry : list) {
            bytes += entry->texture.estimatedBytes();
        }
    }
    return bytes;
}

} // namespace rv::gl
