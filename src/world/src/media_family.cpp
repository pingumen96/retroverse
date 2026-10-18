#include <rv/world/media_family.hpp>

#include <cmath>
#include <set>

#include <rv/core/utf8.hpp>

#include "data_reader.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;
using Json = nlohmann::json;

/// Items from 1 mm to 2 m; gaps, margins and headroom up to 1 m.
constexpr std::int64_t kMaxItemMm = 2'000;
constexpr std::int64_t kMaxClearanceMm = 1'000;

SlotSpec readSlot(const ObjectReader& reader) {
    SlotSpec slot;
    slot.widthMm = reader.integer("widthMm", 1, kMaxItemMm);
    slot.heightMm = reader.integer("heightMm", 1, kMaxItemMm);
    slot.depthMm = reader.integer("depthMm", 1, kMaxItemMm);
    slot.gapMm = reader.integer("gapMm", 0, kMaxClearanceMm);
    slot.marginMm = reader.integer("marginMm", 0, kMaxClearanceMm);
    slot.headroomMm = reader.integer("headroomMm", 0, kMaxClearanceMm);
    return slot;
}

void requireSize(DocumentReader& document, const std::string& pointer, const SlotSpec& slot, std::int64_t width,
                 std::int64_t height, std::int64_t depth, std::string_view orientation) {
    if (!document.failed() && (slot.widthMm != width || slot.heightMm != height || slot.depthMm != depth)) {
        document.fail(pointer, "the " + std::string(orientation) + " slot must be " + std::to_string(width) + " x " +
                                   std::to_string(height) + " x " + std::to_string(depth) +
                                   " mm, the item in that orientation");
    }
}

MediaFamily readFamily(const ObjectReader& reader) {
    MediaFamily family;
    family.id = reader.dataId("id");
    family.item = reader.prefab("item");
    family.itemWidthMm = reader.integer("itemWidthMm", 1, kMaxItemMm);
    family.itemHeightMm = reader.integer("itemHeightMm", 1, kMaxItemMm);
    family.itemDepthMm = reader.integer("itemDepthMm", 1, kMaxItemMm);
    family.spineOut =
        readSlot(reader.object("spineOut", {"depthMm", "gapMm", "headroomMm", "heightMm", "marginMm", "widthMm"}));
    family.faceOut =
        readSlot(reader.object("faceOut", {"depthMm", "gapMm", "headroomMm", "heightMm", "marginMm", "widthMm"}));
    // Stood on its end with its top face outward, the spine is the item's depth wide and its width tall.
    requireSize(reader.document(), reader.pointer("spineOut"), family.spineOut, family.itemDepthMm, family.itemWidthMm,
                family.itemHeightMm, "spine-out");
    requireSize(reader.document(), reader.pointer("faceOut"), family.faceOut, family.itemWidthMm, family.itemHeightMm,
                family.itemDepthMm, "face-out");
    return family;
}

} // namespace

const MediaFamily* MediaFamilyRegistry::find(std::string_view id) const noexcept {
    for (const MediaFamily& family : families) {
        if (family.id == id) {
            return &family;
        }
    }
    return nullptr;
}

Expected<MediaFamilyRegistry> parseMediaFamilies(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    const ObjectReader reader(document, *value, "", {"families", "schemaVersion"});
    (void)reader.integer("schemaVersion", 1, 1);
    MediaFamilyRegistry registry;
    std::set<std::string> ids;
    detail::forEachElement(reader.array("families"), "/families", [&](const Json& element, const std::string& pointer) {
        MediaFamily family = readFamily(
            ObjectReader(document, element, pointer,
                         {"faceOut", "id", "item", "itemDepthMm", "itemHeightMm", "itemWidthMm", "spineOut"}));
        if (!document.failed() && !ids.insert(family.id).second) {
            document.fail(detail::pointerChild(pointer, "id"), "the family appears twice");
        }
        registry.families.push_back(std::move(family));
    });
    if (!document.failed() && registry.families.empty()) {
        document.fail("/families", "must name at least one family");
    }
    if (document.failed()) {
        return propagate(document.error());
    }
    return registry;
}

Expected<MediaFamilyRegistry> loadMediaFamilies(const std::filesystem::path& assetRoot) {
    const std::filesystem::path path = assetRoot / "world" / "media_families.json";
    auto text = detail::readDataFile(path);
    if (!text) {
        return propagate(text.error());
    }
    return parseMediaFamilies(*text, core::pathToUtf8Lossy(path));
}

Status checkItemManifest(const MediaFamily& family, const assets::PrefabManifest& manifest) {
    const std::string context = family.id + ".item";
    if (manifest.id != family.item.manifest) {
        return makeError(ErrorCode::ValidationFailed,
                         "the manifest is \"" + manifest.id + "\", the family names \"" + family.item.manifest + "\"",
                         context);
    }
    if (manifest.runtime.generator.value_or("") != family.item.kind) {
        return makeError(ErrorCode::ValidationFailed,
                         "the manifest builds \"" + manifest.runtime.generator.value_or("") +
                             "\", the family names \"" + family.item.kind + "\"",
                         context);
    }
    if (!manifest.runtime.scaleMeters) {
        return makeError(ErrorCode::ValidationFailed, "the manifest gives no runtime.scaleMeters", context);
    }
    const glm::vec3 meters = *manifest.runtime.scaleMeters;
    const auto millimetres = [](float value) {
        return static_cast<std::int64_t>(std::lround(value * 1000.0));
    };
    const std::int64_t width = millimetres(meters.x);
    const std::int64_t height = millimetres(meters.y);
    const std::int64_t depth = millimetres(meters.z);
    if (width != family.itemWidthMm || height != family.itemHeightMm || depth != family.itemDepthMm) {
        return makeError(ErrorCode::ValidationFailed,
                         "the manifest's scaleMeters is " + std::to_string(width) + " x " + std::to_string(height) +
                             " x " + std::to_string(depth) + " mm, the family's item " +
                             std::to_string(family.itemWidthMm) + " x " + std::to_string(family.itemHeightMm) + " x " +
                             std::to_string(family.itemDepthMm) + " mm",
                         context);
    }
    return ok();
}

} // namespace rv::world
