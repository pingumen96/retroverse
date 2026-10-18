#pragma once

// The media family registry (ADR-0010 §2, style §22's MediaFamilyRegistry): assets/world/media_families.json, how the
// games of a family stand on a shelf. A family names its item's prefab and size and the slots it takes spine-out (the
// item on its end, its top face outward: the spine) and face-out (its front outward); the shelves' slot counts follow
// from these through the prefabs' own slot math (ADR-0010 §5), never from constants in the generator.
//
// The file (JSON with comments; every key required):
//   {"schemaVersion": 1,
//    "families": [{"id": "cartridge_home_16bit",
//                  "item": {"manifest": "media.cartridge.home16.generic", "kind": "cartridge"},
//                  "itemWidthMm": 125, "itemHeightMm": 85, "itemDepthMm": 20,
//                  "spineOut": {"widthMm": 20, "heightMm": 125, "depthMm": 85, "gapMm": 4, "marginMm": 30,
//                               "headroomMm": 20},
//                  "faceOut": {"widthMm": 125, "heightMm": 85, "depthMm": 20, "gapMm": 4, "marginMm": 30,
//                              "headroomMm": 20}}]}
// The item's size is its manifest's runtime.scaleMeters in millimetres (width, height, depth, the front facing +Z),
// which checkItemManifest() proves; a spine-out slot is the item stood on its end (width = the item's depth, height =
// its width, depth = its height), a face-out slot the item as it is.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/prefab_manifest.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>

namespace rv::world {

/// How an item stands in one orientation, in millimetres: the slot it takes (the item's volume in that orientation),
/// the gap between two slots, the margin at each end of a level and the free height kept above the item in its level.
struct SlotSpec {
    std::int64_t widthMm = 0;
    std::int64_t heightMm = 0;
    std::int64_t depthMm = 0;
    std::int64_t gapMm = 0;
    std::int64_t marginMm = 0;
    std::int64_t headroomMm = 0;

    bool operator==(const SlotSpec&) const = default;
};

struct MediaFamily {
    /// A data id ("cartridge_home_16bit").
    std::string id;
    /// The item's prefab.
    PrefabRef item;
    std::int64_t itemWidthMm = 0;
    std::int64_t itemHeightMm = 0;
    std::int64_t itemDepthMm = 0;
    SlotSpec spineOut;
    SlotSpec faceOut;

    bool operator==(const MediaFamily&) const = default;
};

struct MediaFamilyRegistry {
    /// In the file's order, no id twice.
    std::vector<MediaFamily> families;

    /// The family of `id`, or nullptr.
    [[nodiscard]] const MediaFamily* find(std::string_view id) const noexcept;
};

/// Reads the registry. Errors (context and line): ParseError, ValidationFailed (a key missing, unknown, of another type
/// or out of range, an id twice, a slot that is not the item in its orientation).
[[nodiscard]] Expected<MediaFamilyRegistry> parseMediaFamilies(std::string_view text, std::string_view context);
/// Reads <assetRoot>/world/media_families.json. Errors: those of parseMediaFamilies() and of reading the file.
[[nodiscard]] Expected<MediaFamilyRegistry> loadMediaFamilies(const std::filesystem::path& assetRoot);

/// The family's item agrees with its manifest: the manifest's id, its runtime.generator equal to the item's kind, its
/// runtime.scaleMeters rounded to whole millimetres equal to the item's size. Errors: ValidationFailed naming the
/// field.
[[nodiscard]] Status checkItemManifest(const MediaFamily& family, const assets::PrefabManifest& manifest);

} // namespace rv::world
